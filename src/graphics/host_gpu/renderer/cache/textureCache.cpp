#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/dcc_clear_predicate_spv.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"
#include "libs/debugSnapshots.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <cstdio>
#include <mutex>
#include <span>
#include <tuple>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

// Debug: lets the presenter show a guest render target instead of the scan-out surface.
static std::mutex             g_dbg_rt_mutex;
static std::vector<std::pair<uint32_t, uint32_t>> g_dbg_rt_ids;

static std::set<uint64_t> g_dbg_hdr_written;
static std::set<uint64_t> g_dbg_hdr_sampled;

// A guest address is an allocation, not a resource identity: the game recycles one address for
// surfaces of different extent and format, so an address-keyed graph merges unrelated resources
// into one node. Key edges on (address, extent, format) at both ends instead.
struct DebugEdgeKey {
	uint64_t tex_address;
	uint32_t tex_width;
	uint32_t tex_height;
	uint32_t tex_format;
	uint64_t rt_address;
	uint32_t rt_width;
	uint32_t rt_height;
	uint32_t rt_format;

	[[nodiscard]] auto operator<=>(const DebugEdgeKey&) const = default;
};
static std::set<DebugEdgeKey> g_dbg_edges;

// Debug: one line per distinct "this target was produced by sampling that texture" edge.
void DebugRecordEdge(uint64_t tex_address, uint32_t tex_width, uint32_t tex_height,
                     uint32_t tex_format, uint64_t rt_address, uint32_t rt_width,
                     uint32_t rt_height, uint32_t rt_format) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	const DebugEdgeKey key {tex_address, tex_width, tex_height, tex_format,
	                        rt_address,  rt_width,  rt_height,  rt_format};
	if (g_dbg_edges.insert(key).second) {
		LOGF("EDGE: tex=0x%016" PRIx64 " %ux%u fmt=%u -> rt=0x%016" PRIx64 " %ux%u fmt=%u\n",
		     tex_address, tex_width, tex_height, tex_format, rt_address, rt_width, rt_height,
		     rt_format);
	}
}

void DebugRecordHdrWritten(uint64_t address) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	g_dbg_hdr_written.insert(address);
}

void DebugRecordHdrSampled(uint64_t address) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	if (!g_dbg_hdr_sampled.insert(address).second) {
		return;
	}
	// Report the orphans each time the sampled set grows.
	LOGF("HDR SETS: written=%zu sampled=%zu\n", g_dbg_hdr_written.size(),
	     g_dbg_hdr_sampled.size());
	for (const auto written: g_dbg_hdr_written) {
		if (!g_dbg_hdr_sampled.contains(written)) {
			LOGF("  HDR ORPHAN (written, never sampled): 0x%016" PRIx64 "\n", written);
		}
	}
}

void DebugRegisterRenderTargetId(uint32_t id_index, uint32_t id_generation) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	g_dbg_rt_ids.emplace_back(id_index, id_generation);
}

// Guest addresses shift between runs, so the viewer cannot select by address: you would have to
// run once to learn the address, by which time it is stale. Select by format and by index among
// the 1080p targets of that format instead, which is stable enough to cycle through live.
struct DebugRenderTargetEntry {
	uint64_t address;
	uint32_t format;
	uint32_t id_index;
	uint32_t id_generation;
};
static std::vector<DebugRenderTargetEntry> g_dbg_rt_table;

void DebugRegisterRenderTargetAddress(uint64_t address, uint32_t format, uint32_t id_index,
                                      uint32_t id_generation) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	for (auto& entry: g_dbg_rt_table) {
		if (entry.address == address && entry.format == format) {
			// Keep the newest id: the cache recycles slots, and a generation captured once is
			// dead by present time.
			entry.id_index      = id_index;
			entry.id_generation = id_generation;
			return;
		}
	}
	uint32_t index = 0;
	for (const auto& entry: g_dbg_rt_table) {
		if (entry.format == format) {
			index++;
		}
	}
	g_dbg_rt_table.push_back({address, format, id_index, id_generation});
	LOGF("RT REGISTER: fmt=%u index=%u addr=0x%016" PRIx64 "\n", format, index, address);
}

bool DebugFindRenderTarget(uint32_t format, uint32_t index, uint32_t* out_index,
                           uint32_t* out_generation, uint64_t* out_address) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	if (out_index == nullptr || out_generation == nullptr || out_address == nullptr) {
		return false;
	}
	uint32_t seen = 0;
	for (const auto& entry: g_dbg_rt_table) {
		if (entry.format != format) {
			continue;
		}
		if (seen == index) {
			*out_index      = entry.id_index;
			*out_generation = entry.id_generation;
			*out_address    = entry.address;
			return true;
		}
		seen++;
	}
	return false;
}

bool DebugGetRenderTargetId(size_t index, uint32_t* out_index, uint32_t* out_generation) {
	std::scoped_lock lock {g_dbg_rt_mutex};
	if (index >= g_dbg_rt_ids.size() || out_index == nullptr || out_generation == nullptr) {
		return false;
	}
	*out_index      = g_dbg_rt_ids[index].first;
	*out_generation = g_dbg_rt_ids[index].second;
	return true;
}

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	if (cmask ? code == 0 : code == 0x20) {
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid && !PackedColorClearNeedsHighWord(format) &&
		       DecodePackedColorClear(format, metadata.clear_word, 0, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

[[nodiscard]] bool CanTransferStencil(const ImageInfo& info) {
	// Raw buffer copies cannot decode Hi-Stencil or initialize multisampled images.
	return info.samples == 1 && !info.metadata.stencil_compressed;
}

[[nodiscard]] std::vector<vk::BufferImageCopy> BuildDepthCopies(const ImageInfo& info,
                                                              uint64_t slice_stride,
                                                              vk::ImageAspectFlags aspect) {
	std::vector<vk::BufferImageCopy> copies(info.resources.layers);
	for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
		auto& copy             = copies[layer];
		copy.bufferOffset      = slice_stride * layer;
		copy.bufferRowLength   = info.pitch;
		copy.bufferImageHeight = info.extent.height;
		copy.imageSubresource  = {aspect, 0, layer, 1};
		copy.imageExtent       = {info.extent.width, info.extent.height, 1};
	}
	return copies;
}

[[nodiscard]] std::vector<GpuTileInfo> BuildDepthTiles(const ImageInfo& info) {
	TileBlockLayout block {};
	EXIT_NOT_IMPLEMENTED(
	    !TileGetBlockLayout(TileBlockFamily::Depth64KB, info.bytes_per_block, block));
	const auto               full_slice_size = info.data.size / info.resources.layers;
	std::vector<GpuTileInfo> tiles;
	tiles.reserve(info.resources.layers);
	for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
		const auto offset = full_slice_size * layer;
		tiles.push_back({block.family, block.bytes_per_element, offset, full_slice_size, offset,
		                 full_slice_size, 0, info.extent.width, info.extent.height, 1, info.pitch});
		tiles.back().surface_z = layer;
	}
	return tiles;
}

} // namespace

// Debug: correlation clock. A frame number alone cannot order an upload against the GPU writes
// inside the same frame, so every correlated event also carries a monotonic sequence number.
constexpr uint32_t PredicateSlots = 4096;

std::atomic_uint64_t g_dbg_frame {0};
std::atomic_uint64_t g_dbg_seq {0};

nlohmann::json DebugGpuSnapshot() {
	// Frame rate over the interval since the previous snapshot. Several panels may poll at once,
	// so the sample only advances once at least half a second has passed.
	static std::mutex fps_mutex;
	static auto       fps_time  = std::chrono::steady_clock::now();
	static uint64_t   fps_frame = g_dbg_frame.load(std::memory_order_relaxed);
	static double     fps       = 0.0;
	const uint64_t    frame     = g_dbg_frame.load(std::memory_order_relaxed);
	{
		std::scoped_lock lock {fps_mutex};
		const auto       now     = std::chrono::steady_clock::now();
		const double     elapsed = std::chrono::duration<double>(now - fps_time).count();
		if (elapsed >= 0.5) {
			fps       = static_cast<double>(frame - fps_frame) / elapsed;
			fps_time  = now;
			fps_frame = frame;
		}
	}
	nlohmann::json result;
	result["columns"] = {"Format", "Index", "Format name", "Address", "Image id", "Generation"};
	auto& rows        = result["rows"];
	rows              = nlohmann::json::array();
	{
		std::scoped_lock lock {g_dbg_rt_mutex};
		std::map<uint32_t, uint32_t> per_format;
		for (const auto& entry: g_dbg_rt_table) {
			char address[24];
			std::snprintf(address, sizeof(address), "0x%016" PRIx64, entry.address);
			rows.push_back({entry.format, per_format[entry.format]++,
			                vk::to_string(static_cast<vk::Format>(entry.format)), address,
			                entry.id_index, entry.id_generation});
		}
	}
	char fps_text[16];
	std::snprintf(fps_text, sizeof(fps_text), "%.1f", fps);
	result["summary"] = {{"Presented frames", frame},
	                     {"FPS", fps_text},
	                     {"Render targets seen", rows.size()}};
	return result;
}

// EXPERIMENT (diagnostic): force readback enrolment on regardless of the launcher checkbox,
// which did not take effect in testing. Set to false to restore normal config-driven behaviour.
static constexpr bool kKytyExperimentForceReadback = false;

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled() ||
                               kKytyExperimentForceReadback) {
	// Debug: readback enrolment is gated on this, so its value decides whether the tiled-download
	// experiment is active at all.
	LOGF("READBACK CONFIG: readback_linear_images=%d\n", m_readback_linear_images ? 1 : 0);
	if (m_graphics.CanReportMemoryUsage()) {
		ConfigureGarbageCollectionBudget(m_graphics.GetTotalMemoryBudget());
	}
}

void TextureCache::ConfigureGarbageCollectionBudget(uint64_t available_budget) {
	constexpr int64_t GiB = 1024ll * 1024 * 1024;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(available_budget, INT64_MAX));
	const auto threshold = std::min<int64_t>(budget, 8 * GiB);
	m_pressure_gc_memory = static_cast<uint64_t>(
	    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - GiB), GiB + GiB / 2));
	m_critical_gc_memory = static_cast<uint64_t>(
	    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - GiB / 2), 3 * GiB));
	// Keep reusable images resident until memory is actually under pressure. The earlier trigger
	// collected at roughly half the budget, which can reclaim a render target that a later pass
	// still samples.
	m_trigger_gc_memory = m_pressure_gc_memory;
	if (const auto* legacy = std::getenv("KYTY_TEXTURE_CACHE_EARLY_GC");
	    legacy != nullptr && std::strcmp(legacy, "1") == 0) {
		m_trigger_gc_memory = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	}
	LOGF("TEXTURE GC BUDGET: trigger=%" PRIu64 " pressure=%" PRIu64 " critical=%" PRIu64 "\n",
	     m_trigger_gc_memory, m_pressure_gc_memory, m_critical_gc_memory);
}

TextureCache::~TextureCache() {
	if (m_clear_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_clear_pipeline, nullptr);
		m_graphics.device.destroyPipelineLayout(m_clear_pipeline_layout, nullptr);
		m_graphics.device.destroyDescriptorSetLayout(m_clear_desc_layout, nullptr);
	}
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format) ||
	    cached.type != requested.type) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

bool TextureCache::SafeToDownload(const Image& image) {
	if (!image.SafeToDownload()) {
		return false;
	}
	const auto range = image.info.data;
	if (!m_buffer_cache.HasGpuDirtyBytes(range.address, range.size)) {
		return true;
	}
	// Image-local state alone is not enough here: a buffer write that landed on these bytes after
	// the image was written makes the image stale, and reading it back hands the guest old data.
	// On Beast of Reincarnation that stale readback left a shader spinning until the device was
	// lost. The transient allocator does reuse one range for unrelated surfaces, though, so
	// KYTY_IMAGE_TICK_ARBITRATION lets the newer of the two writes own the bytes instead.
	static const bool arbitrate = [] {
		const char* value = std::getenv("KYTY_IMAGE_TICK_ARBITRATION");
		return value != nullptr && value[0] != '0';
	}();
	if (!arbitrate) {
		return false;
	}
	return image.gpu_write_tick > m_buffer_cache.GpuWriteTick(range.address, range.size);
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
	const auto id = m_slot_images.insert(m_graphics, m_scheduler, info);
	if (!info.data.Empty()) {
		RegisterImage(id);
	}
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		m_image_page_table[page].push_back(id);
	});
	image.registered = true;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id) {
	auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) {
		return;
	}
	if (!image->depth_id) {
		std::vector<ImageId> associations;
		m_slot_images.ForEach([&](ImageId candidate, const Image& associated) {
			if (associated.depth_id == id) {
				associations.push_back(candidate);
			}
		});
		for (const auto association: associations) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.HasMetadata()) {
		const auto metadata = m_surface_metas.find(image->info.metadata.range.address);
		if (metadata != m_surface_metas.end() &&
		    image->info.metadata.kind == ImageMetadataKind::Htile &&
		    metadata->second.type == MetaDataInfo::Type::HTile) {
			// A later binding may have reused this address for another metadata type.
			m_surface_metas.erase(metadata);
		}
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	PreserveStencil(id);
	auto& image = m_slot_images[id];
	if (image.IsGpuModified()) {
		image.ClearGpuModified();
	}
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	image.track_addr   = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	image.track_addr_end = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	// EXPERIMENT (diagnostic, not a fix): the `!image.info.IsTiled()` condition used to stand
	// here, which excluded every render target, since they are all tiled. The game renders with
	// the GPU and then reads that memory back as a texture, so with tiled targets barred from
	// readback the guest range is never populated and the later UploadImage supplies stale RAM.
	// One run measured 667 guest->image uploads against a single image->guest download. Dropping
	// the condition lets tiled targets enrol, to test whether that closes the loop. It detiles
	// and reads back GPU targets, which is why the condition existed: revert if the cost is
	// unacceptable. Still gated by the readback_linear_images config flag.
	// Narrowed to 1080p targets: enrolling every GPU-modified image exhausts the reusable
	// download buffer ("failed to map reusable download buffer"). The hypothesis only concerns
	// the 1920x1080 surfaces that feed the displayed texture, so test those alone.
	const bool dbg_experiment_shape =
	    image.info.extent.width == 1920 && image.info.extent.height == 1080;
	if (m_readback_linear_images && !image.info.data.Empty() &&
	    (!image.info.IsTiled() || dbg_experiment_shape)) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

ImageId TextureCache::GetNullImage(const ImageDesc& desc) {
	const auto format = desc.info.pixel_format;
	if (const auto found = m_null_images.find(format); found != m_null_images.end()) {
		return found->second;
	}
	ImageInfo info {};
	info.pixel_format    = desc.info.pixel_format;
	info.guest_format    = desc.info.guest_format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = std::max(desc.info.bytes_per_block, 1u);
	info.samples         = 1;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.mip_layout[0]   = {0, info.bytes_per_block, 1, 1};
	const auto id        = InsertImage(info);
	m_null_images.emplace(format, id);

	// A null descriptor must read as zero. Leaving the backing uninitialised samples whatever the
	// allocation happened to contain, which reaches the screen as solid white blocks.
	auto& command = m_scheduler.Current();
	if (!command.IsInvalid()) {
		auto&                     null_image = m_slot_images[id];
		vk::ImageSubresourceRange range {};
		range.aspectMask     = null_image.info.IsDepth()
		                           ? ImageViewOps::DepthAspectMask(null_image.backing.format)
		                           : vk::ImageAspectFlagBits::eColor;
		range.baseMipLevel   = 0;
		range.levelCount     = 1;
		range.baseArrayLayer = 0;
		range.layerCount     = 1;
		// Value-initialised: zero for colour and for depth/stencil alike.
		const vk::ClearValue clear {};
		ClearImage(command, id, null_image.backing.format, range, clear);
	}
	return id;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	const bool source_depth      = source.info.IsDepth();
	const bool destination_depth = destination.info.IsDepth();
	if (source_depth == destination_depth) {
		return false;
	}
	auto&      depth          = source_depth ? source : destination;
	auto&      color          = source_depth ? destination : source;
	const auto transfer_bytes = DepthAspectTransferBytes(depth.backing.format);
	if (depth.info.bytes_per_block != sizeof(uint16_t) ||
	    color.info.bytes_per_block != sizeof(uint16_t) || transfer_bytes != sizeof(uint32_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto     layers = depth.info.resources.layers;
	const uint64_t depth_slice =
	    static_cast<uint64_t>(depth.info.pitch) * depth.info.extent.height * transfer_bytes;
	const uint64_t color_slice =
	    static_cast<uint64_t>(color.info.pitch) * color.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || depth_slice > UINT64_MAX / layers || color_slice > UINT64_MAX / layers);
	const auto                       depth_size = depth_slice * layers;
	const auto                       color_size = color_slice * layers;
	std::vector<vk::BufferImageCopy> depth_copies(layers);
	std::vector<vk::BufferImageCopy> color_copies(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		depth_copies[layer].bufferOffset      = depth_slice * layer;
		depth_copies[layer].bufferRowLength   = depth.info.pitch;
		depth_copies[layer].bufferImageHeight = depth.info.extent.height;
		depth_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
		depth_copies[layer].imageExtent       = depth.info.extent;
		color_copies[layer].bufferOffset      = color_slice * layer;
		color_copies[layer].bufferRowLength   = color.info.pitch;
		color_copies[layer].bufferImageHeight = color.info.extent.height;
		color_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, layer, 1};
		color_copies[layer].imageExtent       = color.info.extent;
	}

	auto                         depth_buffer = m_tiler.GetScratchBuffer(depth_size);
	auto                         color_buffer = m_tiler.GetScratchBuffer(color_size, depth_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = depth.info.extent.width,
	    .height              = depth.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(color.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(depth.info.pitch) * transfer_bytes,
	    .source_slice_stride = color_slice,
	    .target_slice_stride = depth_slice,
	};
	const bool d32 = DepthAspectTransferFormat(depth.backing.format) == vk::Format::eD32Sfloat;
	if (source_depth) {
		source.Download(depth_copies, depth_buffer.buffer, depth_buffer.offset, depth_buffer.size);
		m_tiler.ConvertD16(depth_buffer, color_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(color_copies, color_buffer.buffer, color_buffer.offset,
		                   color_buffer.size);
	} else {
		source.Download(color_copies, color_buffer.buffer, color_buffer.offset, color_buffer.size);
		m_tiler.ConvertD16(color_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	if (direct_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer);
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264
	const bool raw_d16_texture =
	    binding == BindingType::Texture && cached.info.IsDepth() &&
	    cached.info.guest_format == Prospero::BufferFormat::k16UNorm &&
	    requested.guest_format == Prospero::BufferFormat::k16UInt &&
	    requested.pixel_format == vk::Format::eR16Uint && cached.backing.samples == 1 &&
	    requested.samples == 1 && requested.data == cached.info.data &&
	    requested.extent == cached.info.extent && requested.resources == cached.info.resources &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	// PPSA04264
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	bool recreate = cached.info.resources < requested.resources;
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_d16_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout = cached.info.mip_layout;
	} else {
		info.resources = std::max(requested.resources, cached.info.resources);
	}
	info.htile_clear_mask     = 0;
	const auto replacement_id = InsertImage(info);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	FreeImage(cached_id);
	return replacement_id;
}

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr) {
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.data.size == cached.info.data.size &&
		    (requested.IsVolume() || cached.info.IsVolume())) {
			return {ExpandImage(requested, cached_id)};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     requested.mip_layout != cached.info.mip_layout)) {
			if (safe_to_delete) {
				FreeImage(cached_id);
			}
			return {merged_id};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode));
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address && safe_to_delete) {
		FreeImage(cached_id);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info);
	auto&      expanded    = m_slot_images[expanded_id];
	auto&      source      = m_slot_images[source_id];
	expanded.usage         = source.usage;
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

struct TextureCache::TextureTransfer {
	TextureUploadLayout              layout;
	std::vector<vk::BufferImageCopy> regions;
	std::vector<GpuTileInfo>         tiles;
	bool                             swap_bgra16 = false;
	bool                             valid       = false;

	[[nodiscard]] uint64_t LinearSize() const {
		uint64_t size = 0;
		for (const auto& tile: tiles) {
			size = std::max(size, tile.linear_offset + tile.linear_size);
		}
		return size;
	}
};

struct TextureCache::ImageDownload {
	TextureTransfer texture;
	bool                depth_target = false;
	bool                valid        = false;
};

TextureCache::TextureTransfer
TextureCache::BuildTextureTransfer(const Image& image, BindingType binding,
                                    TransferDirection direction) const {
	const auto& info             = image.info;
	const bool  upload           = direction == TransferDirection::Upload;
	const bool  render_target    = binding == BindingType::RenderTarget;
	const bool  video_out        = binding == BindingType::VideoOut;
	auto        format           = info.guest_format;
	uint32_t    layers           = info.TransferLayers();
	bool        volume           = info.IsVolume();
	bool        allow_depth_tile = upload;
	const char* owner            = "TextureCache readback";

	TextureTransfer transfer;
	transfer.swap_bgra16 = info.bgra16 && (!upload || render_target || video_out);
	// A render target can alias a block-compressed image through a block-texel view; that image's
	// extent is in texels, not blocks, so it keeps its own format and layout.
	if (render_target && !info.IsBlock()) {
		format = ImageOps::RenderTargetTransferFormat(info.bytes_per_block);
	}
	if (video_out) {
		allow_depth_tile = false;
	} else if (render_target || binding == BindingType::Storage) {
		allow_depth_tile = true;
	}
	if (upload) {
		if ((render_target || video_out) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		owner = "TextureCache";
		if (render_target) {
			owner = "RenderTarget";
		} else if (binding == BindingType::Storage) {
			owner = "StorageTextureCache";
		} else if (video_out) {
			if (info.metadata.compression != VideoOutCompression::Uncompressed) {
				EXIT("TextureCache: invalid color-attachment upload\n");
			}
			layers = info.resources.layers;
			volume = false;
			owner  = "VideoOut";
		}
	}

	transfer.layout  = TextureCalcUploadLayout(format, info.extent.width, info.extent.height,
	                                       info.resources.levels, layers, info.tile_mode,
	                                       info.data.size, allow_depth_tile, volume, owner);
	transfer.regions = TextureBuildImageCopies(transfer.layout);
	if (info.IsDepth()) {
		for (auto& region: transfer.regions) {
			region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
		}
	}
	if (transfer.layout.surface.description.tile_mode != Prospero::TileMode::kLinear) {
		if (!TextureBuildGpuTileInfos(info.data.size, transfer.regions, transfer.layout,
		                              info.resources.levels, transfer.tiles)) {
			return transfer;
		}
	}
	transfer.valid = true;
	return transfer;
}

TextureCache::ImageDownload TextureCache::BuildDownload(const Image& image) const {
	const auto&  info    = image.info;
	const auto   binding = UploadBinding(image);
	ImageDownload transfer {.depth_target = binding == BindingType::DepthTarget};
	if (info.samples != 1 || image.backing.samples != 1) {
		return transfer;
	}
	if (transfer.depth_target) {
		transfer.valid = IsSupportedDepthPlaneReadback(info) && info.resources.layers != 0 &&
		             info.data.size % info.resources.layers == 0 &&
		             Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
		return transfer;
	}
	if (info.metadata.compression != VideoOutCompression::Uncompressed) {
		return transfer;
	}
	transfer.texture = BuildTextureTransfer(image, binding, TransferDirection::Download);
	transfer.valid   = transfer.texture.valid;
	return transfer;
}

void TextureCache::UploadImage(Image& image, Buffer& source, uint64_t source_offset) {
	auto& destination = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto binding = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	// Debug: this is the guest-memory -> image path. An instance with no GPU producer and no
	// upload has no source at all; one with an upload is externally supplied data. Keyed by
	// (address, extent, format) so it lines up with the shape-keyed graph.
	// NOT deduplicated: collapsing occurrences is what made an earlier pass unable to tell
	// allocation reuse over time from genuine simultaneous aliasing. Restricted to 1080p so the
	// volume stays usable.
	// 1620 rows is a 1920x1080 NV12 frame seen as one R8 plane, which is how the decoded
	// movie is bound; include it so the movie texture's upload cadence is visible.
	if (const auto& info = image.info;
	    DebugGfxTraceEnabled() && info.extent.width == 1920 &&
	    (info.extent.height == 1080 || info.extent.height == 1620)) {
		LOGF("CONSUME: frame=%" PRIu64 " seq=%" PRIu64 " addr=0x%016" PRIx64 " size=0x%" PRIx64
		     " %ux%u fmt=%u guest_fmt=%u mips=%u layers=%u binding=%u\n",
		     g_dbg_frame.load(std::memory_order_relaxed),
		     g_dbg_seq.fetch_add(1, std::memory_order_relaxed), info.data.address, info.data.size,
		     info.extent.width, info.extent.height, static_cast<uint32_t>(image.backing.format),
		     static_cast<uint32_t>(info.guest_format), info.resources.levels,
		     info.resources.layers, static_cast<uint32_t>(binding));
	}
	const auto  upload  =[&](std::vector<vk::BufferImageCopy>& copies, TileManager::Result linear) {
		for (auto& copy: copies) {
			copy.bufferOffset += linear.offset;
		}
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
	};

	if (binding != BindingType::DepthTarget) {
		const auto& info = image.info;
		auto transfer = BuildTextureTransfer(image, binding, TransferDirection::Upload);
		if (!transfer.valid) {
			EXIT("TextureCache: invalid texture upload: binding=%u addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " format=%u tile=%u family=%u extent=%ux%ux%u "
			     "pitch=%u levels=%u layers=%u samples=%u\n",
			     static_cast<uint32_t>(binding), info.data.address, info.data.size,
			     static_cast<uint32_t>(info.guest_format), static_cast<uint32_t>(info.tile_mode),
			     static_cast<uint32_t>(transfer.layout.surface.texture.block.family), info.extent.width,
			     info.extent.height, info.extent.depth, info.pitch, info.resources.levels,
			     info.resources.layers, info.samples);
		}
		TileManager::Result linear {source.Handle(), source_offset, info.data.size};
		if (!transfer.tiles.empty()) {
			linear = m_tiler.Detile(source.Handle(), source_offset, info.data.size,
			                        transfer.LinearSize(), transfer.tiles);
		}
		if (transfer.swap_bgra16) {
			linear = m_tiler.SwapBgra16(linear);
		}
		upload(transfer.regions, linear);
		return;
	}

	// The stencil plane has its own row pitch and shares the native image
	// with the depth plane.
	auto info = destination.info;
	if (image.depth_id) {
		info.data            = image.info.data;
		info.guest_format    = Prospero::BufferFormat::k8UInt;
		info.bytes_per_block = 1;
		if (info.IsTiled()) info.pitch = TileGetDepthPitch(info.extent.width, 1, 0);
	}
	if (info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	const auto          layers          = info.resources.layers;
	const auto          full_slice_size = info.data.size / layers;
	auto copies = BuildDepthCopies(info, full_slice_size, image.depth_id
	                                                        ? vk::ImageAspectFlagBits::eStencil
	                                                        : vk::ImageAspectFlagBits::eDepth);
	TileManager::Result linear {source.Handle(), source_offset, source.Size() - source_offset};
	if (info.IsTiled()) {
		const auto tiles = BuildDepthTiles(info);
		linear =
		    m_tiler.Detile(source.Handle(), source_offset, info.data.size, info.data.size, tiles);
	}
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		for (uint32_t layer = 0; layer < layers; layer++) {
			copies[layer].bufferOffset = transfer_slice * layer;
		}
	}
	upload(copies, linear);
}

void TextureCache::TransferStencil(Image& image, GuestRange stencil, Buffer& buffer,
                                  uint64_t offset, TransferDirection direction) {
	auto info            = image.info;
	info.data            = stencil;
	info.bytes_per_block = 1;
	// PS5 stores stencil separately, with its own one-byte depth-tile pitch.
	if (info.IsTiled()) {
		info.pitch = TileGetDepthPitch(info.extent.width, 1, 0);
	}
	EXIT_IF(info.resources.levels != 1 || info.resources.layers == 0 ||
	        stencil.size % info.resources.layers != 0);
	const auto slice_size = stencil.size / info.resources.layers;
	EXIT_IF(static_cast<uint64_t>(info.pitch) * info.extent.height > slice_size);
	auto copies = BuildDepthCopies(info, slice_size, vk::ImageAspectFlagBits::eStencil);
	const bool          upload = direction == TransferDirection::Upload;
	TileManager::Result linear {buffer.Handle(), offset, stencil.size};
	if (info.IsTiled()) {
		const auto tiles = BuildDepthTiles(info);
		if (!upload) {
			m_tiler.TileImage(image, copies, buffer.Handle(), offset, stencil.size, stencil.size,
			                  tiles);
			return;
		}
		linear = m_tiler.Detile(buffer.Handle(), offset, stencil.size, stencil.size, tiles);
	}
	for (auto& copy: copies) {
		copy.bufferOffset += linear.offset;
	}
	if (upload) {
		image.Upload(copies, linear.buffer, linear.offset, linear.size);
	} else {
		image.Download(copies, linear.buffer, linear.offset, linear.size);
	}
}

// A depth image about to be retired still owns its stencil plane. Publish those bytes back to
// the association before the image goes, or the next bind uploads a stale plane over them.
void TextureCache::PreserveStencil(ImageId depth_id) {
	auto& depth = m_slot_images[depth_id];
	if (depth.depth_id || !depth.info.HasStencil() || !CanTransferStencil(depth.info)) {
		return;
	}
	const auto stencil = depth.info.stencil;
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		auto& record = m_slot_images[id];
		if (record.depth_id != depth_id || record.info.data != stencil || record.IsCpuDirty() ||
		    record.IsBufferModified()) {
			continue;
		}
		auto [buffer, offset] =
		    m_buffer_cache.ObtainBuffer(stencil.address, stencil.size, true, false);
		EXIT_IF(buffer == nullptr);
		TransferStencil(depth, stencil, *buffer, offset, TransferDirection::Download);
		record.MarkBufferModified();
	}
}

// A transient render-target pool reuses one guest address for surfaces of different formats. When
// one of them writes, the others are marked dirty and would re-upload guest memory into
// themselves, reinterpreting the writer's bytes through their own format. Uploading across
// formats is never right, so report when some other GPU-owned image of a different shape holds
// this range.
// Debug: every guest->image upload of a full-size surface, with the verdict of each guard and
// the shape currently recorded as owning those bytes. Says whether a stomping upload was even
// offered to the guards, which a skip counter alone cannot.
void TextureCache::DebugReportStaleBind(ImageId id, uint64_t rt_address) {
	// Taking the lock and walking the page table on every bind costs more than the frame it
	// measures, so spend a fixed budget and then do nothing at all.
	static std::atomic<uint64_t> budget {0};
	if (budget.fetch_add(1, std::memory_order_relaxed) >= 20000) {
		return;
	}
	std::scoped_lock lock {m_lock};
	const auto* bound = m_slot_images.try_get(id);
	if (bound == nullptr || bound->info.extent.width < 640 || bound->IsGpuModified()) {
		return;
	}
	// The bound image holds no GPU output. If a sibling over the same bytes does, this pass is
	// reading the wrong one.
	for (const auto other_id: FindImagesInRegion(bound->info.data.address,
	                                             bound->info.data.size, false)) {
		if (other_id == id) {
			continue;
		}
		const auto* other = m_slot_images.try_get(other_id);
		if (other == nullptr || !other->IsGpuModified() ||
		    other->info.extent.width != bound->info.extent.width) {
			continue;
		}
		static std::atomic<uint64_t> stale {0};
		const auto n = stale.fetch_add(1, std::memory_order_relaxed);
		if (n < 40 || (n % 2048) == 0) {
			LOGF("STALE BIND #%" PRIu64 ": bound id=%u addr=0x%016" PRIx64 " size=0x%" PRIx64
			     " %ux%u fmt=%u levels=%u layers=%u tile=%u | owner id=%u addr=0x%016" PRIx64
			     " size=0x%" PRIx64 " %ux%u fmt=%u levels=%u layers=%u tile=%u | rt=0x%016" PRIx64
			     "\n",
			     n, id.index, bound->info.data.address, bound->info.data.size,
			     bound->info.extent.width, bound->info.extent.height,
			     static_cast<uint32_t>(bound->backing.format), bound->info.resources.levels,
			     bound->info.resources.layers, static_cast<uint32_t>(bound->info.tile_mode),
			     other_id.index, other->info.data.address, other->info.data.size,
			     other->info.extent.width, other->info.extent.height,
			     static_cast<uint32_t>(other->backing.format), other->info.resources.levels,
			     other->info.resources.layers, static_cast<uint32_t>(other->info.tile_mode),
			     rt_address);
		}
		return;
	}
}

// Debug: one line per distinct owner/outcome. `skip` is null when the publication happened, so a
// frame that still shows stale pixels can be traced to either "no owner published" or the reason
// the owner was refused.
static void DebugReportOwnerPublish(ImageId destination, ImageId owner_id, const Image& owner,
                                    const char* skip, uint64_t buffer_tick) {
	if (owner.info.extent.width < 640) {
		return;
	}
	static std::mutex                                                     dbg_mutex;
	static std::set<std::tuple<uint64_t, uint32_t, uint32_t, std::string>> dbg_seen;
	const std::tuple<uint64_t, uint32_t, uint32_t, std::string> key {
	    owner.info.data.address, owner.info.extent.width,
	    static_cast<uint32_t>(owner.backing.format), skip == nullptr ? "published" : skip};
	bool first = false;
	{
		std::scoped_lock lock {dbg_mutex};
		first = dbg_seen.insert(key).second;
	}
	if (!first) {
		return;
	}
	LOGF("OWNER PUBLISH: dst=%u owner=%u addr=0x%016" PRIx64 " size=0x%" PRIx64 " %ux%u fmt=%u"
	     " gpu_write=%" PRIu64 " guest_sync=%" PRIu64 " image_tick=%" PRIu64
	     " buffer_tick=%" PRIu64 " image_newer=%d outcome=%s\n",
	     static_cast<uint32_t>(destination.index), static_cast<uint32_t>(owner_id.index),
	     owner.info.data.address, owner.info.data.size, owner.info.extent.width,
	     owner.info.extent.height, static_cast<uint32_t>(owner.backing.format),
	     owner.GpuWriteSerial(), owner.GuestSyncSerial(), owner.gpu_write_tick, buffer_tick,
	     owner.gpu_write_tick > buffer_tick ? 1 : 0, skip == nullptr ? "published" : skip);
}

// The guest address of an image is not an identity: a transient allocator reuses and overlaps
// offsets, so the live pixels for a range can sit inside a different image entirely. Before
// anything reconstructs an image from guest memory, hand the bytes to their owner to fill in.
uint32_t TextureCache::PublishGpuOwners(ImageId destination, GuestRange range) {
	// Opt-in for the same reason as the tick arbitration above.
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_IMAGE_OWNER_PUBLISH");
		return value != nullptr && value[0] != '0';
	}();
	if (!enabled || !range.Valid()) {
		return 0;
	}
	// Debug: without these the probe below is silent both when nothing owns the bytes and when
	// every owner is filtered out, which are different answers.
	static std::atomic_uint64_t dbg_calls {0};
	static std::atomic_uint64_t dbg_candidates {0};
	static std::atomic_uint64_t dbg_self {0};
	static std::atomic_uint64_t dbg_unusable {0};
	static std::atomic_uint64_t dbg_not_gpu {0};
	static std::atomic_uint64_t dbg_current {0};
	static std::atomic_uint64_t dbg_no_overlap {0};
	static std::atomic_uint64_t dbg_compressed {0};
	static std::atomic_uint64_t dbg_published {0};
	const auto dbg_call_index = dbg_calls.fetch_add(1, std::memory_order_relaxed);
	if ((dbg_call_index % 4096) == 0) {
		LOGF("OWNER PUBLISH STATS: calls=%" PRIu64 " candidates=%" PRIu64 " self=%" PRIu64
		     " unusable=%" PRIu64 " not_gpu=%" PRIu64 " guest_current=%" PRIu64
		     " no_overlap=%" PRIu64 " compressed=%" PRIu64 " published=%" PRIu64 "\n",
		     dbg_call_index, dbg_candidates.load(std::memory_order_relaxed),
		     dbg_self.load(std::memory_order_relaxed), dbg_unusable.load(std::memory_order_relaxed),
		     dbg_not_gpu.load(std::memory_order_relaxed),
		     dbg_current.load(std::memory_order_relaxed),
		     dbg_no_overlap.load(std::memory_order_relaxed),
		     dbg_compressed.load(std::memory_order_relaxed),
		     dbg_published.load(std::memory_order_relaxed));
	}
	uint32_t published = 0;
	for (const auto owner_id: FindImagesInRegion(range.address, range.size, false)) {
		dbg_candidates.fetch_add(1, std::memory_order_relaxed);
		if (owner_id == destination) {
			dbg_self.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		auto* owner = m_slot_images.try_get(owner_id);
		if (owner == nullptr || owner->depth_id || owner->backing.image == nullptr) {
			dbg_unusable.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		if (!owner->IsGpuModified()) {
			dbg_not_gpu.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		if (owner->GuestMemoryIsCurrent()) {
			dbg_current.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		if (!owner->Overlaps(range.address, range.size)) {
			dbg_no_overlap.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		if (owner->info.metadata.compression != VideoOutCompression::Uncompressed) {
			dbg_compressed.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		const char* skip = nullptr;
		if (!SafeToDownload(*owner)) {
			skip = owner->IsBufferModified() ? "buffer-modified"
			       : owner->IsCpuDirty()     ? "cpu-dirty"
			                                 : "buffer-dirty-bytes";
		}
		auto transfer = skip == nullptr ? BuildDownload(*owner) : ImageDownload {};
		if (skip == nullptr && !transfer.valid) {
			skip = "no-transfer";
		}
		std::pair<Buffer*, uint64_t> target {nullptr, 0};
		if (skip == nullptr) {
			// The write flag is what legitimately hands these bytes to the GPU side: it flushes
			// any outstanding CPU pages into the buffer first and then moves the tracker's dirty
			// state from CPU to GPU, which a direct mark would corrupt.
			target = m_buffer_cache.ObtainBuffer(owner->info.data.address, owner->info.data.size,
			                                     true, false);
			// A small read can be served from the upload stream, which is not a copy destination.
			if (target.first == nullptr ||
			    !target.first->IsInBounds(owner->info.data.address, owner->info.data.size)) {
				skip = "no-buffer";
			}
		}
		const auto buffer_tick =
		    m_buffer_cache.GpuWriteTick(owner->info.data.address, owner->info.data.size);
		DebugReportOwnerPublish(destination, owner_id, *owner, skip, buffer_tick);
		// Debug: how often the refusing buffer claim is older than the image's own GPU write,
		// i.e. how often the boolean gate is discarding the newer contents.
		if (skip != nullptr && std::string_view {skip} == "buffer-dirty-bytes") {
			static std::atomic_uint64_t dbg_stale_claim {0};
			static std::atomic_uint64_t dbg_fresh_claim {0};
			auto& counter = owner->gpu_write_tick > buffer_tick ? dbg_stale_claim : dbg_fresh_claim;
			const auto n  = counter.fetch_add(1, std::memory_order_relaxed);
			if ((n % 2048) == 0) {
				LOGF("OWNER CLAIM STATS: image_newer=%" PRIu64 " buffer_newer=%" PRIu64 "\n",
				     dbg_stale_claim.load(std::memory_order_relaxed),
				     dbg_fresh_claim.load(std::memory_order_relaxed));
			}
		}
		if (skip != nullptr) {
			continue;
		}
		DownloadImage(*owner, *target.first, target.second, owner->info.data.size,
		              std::move(transfer));
		owner->MarkGuestSynchronized();
		dbg_published.fetch_add(1, std::memory_order_relaxed);
		published++;
	}
	return published;
}

void TextureCache::InitializeImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.info.data.Empty()) {
		return;
	}
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	const auto& target = image.depth_id ? m_slot_images[image.depth_id] : image;
	// Multisampled images cannot be initialized with a buffer-to-image copy.
	if (target.info.samples > 1) {
		return;
	}
	if (image.depth_id &&
	    (!CanTransferStencil(target.info) || image.info.data != target.info.stencil)) {
		return;
	}
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	// Debug: how often an image is rebuilt from guest memory at all, and how much of that is the
	// large surfaces the scene passes through.
	{
		static std::atomic_uint64_t dbg_init {0};
		static std::atomic_uint64_t dbg_upload {0};
		static std::atomic_uint64_t dbg_upload_big {0};
		const auto n = dbg_init.fetch_add(1, std::memory_order_relaxed);
		if (upload) {
			dbg_upload.fetch_add(1, std::memory_order_relaxed);
			if (image.info.extent.width >= 640) {
				dbg_upload_big.fetch_add(1, std::memory_order_relaxed);
			}
		}
		if ((n % 8192) == 0) {
			LOGF("INIT IMAGE STATS: calls=%" PRIu64 " uploads=%" PRIu64 " uploads_big=%" PRIu64
			     "\n", n, dbg_upload.load(std::memory_order_relaxed),
			     dbg_upload_big.load(std::memory_order_relaxed));
		}
	}
	if (upload) {
		// Whoever holds GPU output for these bytes publishes it first; otherwise the upload below
		// reads whatever stale copy guest memory still carries.
		(void)PublishGpuOwners(id, image.info.data);
		const auto [source, source_offset] =
		    m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
		if (source == nullptr) {
			EXIT("TextureCache: failed to obtain image upload source\n");
		}
		if (image.depth_id) {
			TransferStencil(m_slot_images[image.depth_id], image.info.data, *source, source_offset,
			                TransferDirection::Upload);
		} else {
			UploadImage(image, *source, source_offset);
		}
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

bool TextureCache::PaintColorClearOnGpu(ImageId id, const ImageDesc& desc, uint32_t first,
                                        uint32_t image_first, uint32_t count, uint32_t layers) {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DCC_GPU_PAINT");
		return value == nullptr || value[0] != '0';
	}();
	const auto& metadata = desc.info.metadata;
	if (!enabled || !m_graphics.conditional_rendering_enabled ||
	    (metadata.kind != ImageMetadataKind::Dcc && metadata.kind != ImageMetadataKind::Cmask) ||
	    desc.view_info.level_count != 1) {
		return false;
	}
	// Candidate keys: the four constant DCC clears and the register clear. A CMASK clear is the
	// all-zero key, which DecodeColorClear resolves to the register clear.
	constexpr uint8_t dcc_keys[]   = {0x00, 0x40, 0x80, 0xc0, 0x20};
	constexpr uint8_t cmask_keys[] = {0x00};
	const auto        keys         = metadata.kind == ImageMetadataKind::Cmask
	                                     ? std::span<const uint8_t> {cmask_keys}
	                                     : std::span<const uint8_t> {dcc_keys};
	uint32_t          candidates[8] {};
	vk::ClearValue    clears[8] {};
	uint32_t          candidate_count = 0;
	for (const auto key: keys) {
		vk::ClearValue clear {};
		if (DecodeColorClear(desc, key, clear.color)) {
			candidates[candidate_count] = key;
			clears[candidate_count]     = clear;
			candidate_count++;
		}
	}
	if (candidate_count == 0) {
		return true;
	}

	if (m_clear_pipeline == nullptr) {
		const vk::DescriptorSetLayoutBinding bindings[] {
		    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
		    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
		};
		vk::DescriptorSetLayoutCreateInfo layout_info {};
		layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		layout_info.bindingCount = std::size(bindings);
		layout_info.pBindings    = bindings;
		RequireVulkanSuccess(
		    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_clear_desc_layout),
		    "create DCC clear descriptor layout");
		const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0,
		                                  12 * sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo pipeline_layout_info {};
		pipeline_layout_info.setLayoutCount         = 1;
		pipeline_layout_info.pSetLayouts            = &m_clear_desc_layout;
		pipeline_layout_info.pushConstantRangeCount = 1;
		pipeline_layout_info.pPushConstantRanges    = &push;
		RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
		                                                            &m_clear_pipeline_layout),
		                     "create DCC clear pipeline layout");
		const auto module = CompileSPV(DCC_CLEAR_PREDICATE_SPV, m_graphics.device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage  = stage;
		pipeline_info.layout = m_clear_pipeline_layout;
		const auto result    = m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info,
		                                                                nullptr, &m_clear_pipeline);
		m_graphics.device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create DCC clear pipeline");
		m_clear_predicates = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer |
		        vk::BufferUsageFlagBits::eConditionalRenderingEXT |
		        vk::BufferUsageFlagBits::eTransferDst,
		    PredicateSlots * 8 * sizeof(uint32_t));
	}

	const auto range      = metadata.range;
	const auto slice_size = range.size / layers;
	// The keys stay on the GPU: obtain them as written, since the pass consumes matched slices.
	auto [meta_buffer, meta_offset] =
	    m_buffer_cache.ObtainBuffer(range.address, range.size, true, false);
	const auto alignment      = m_graphics.StorageMinAlignment();
	const auto aligned_offset = Common::AlignDown(meta_offset, alignment);
	const auto lead           = meta_offset - aligned_offset;
	if (lead % sizeof(uint32_t) != 0 || slice_size % sizeof(uint32_t) != 0) {
		return false;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto cmd = command.Handle();

	vk::MemoryBarrier2 before {};
	before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	before.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &before;
	cmd.pipelineBarrier2(dependency);

	const vk::DescriptorBufferInfo infos[] {
	    {meta_buffer->Handle(), aligned_offset, lead + range.size},
	    {m_clear_predicates->Handle(), 0, m_clear_predicates->Size()},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_clear_pipeline);
	cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_clear_pipeline_layout, 0, writes);
	std::vector<uint32_t> slots(count);
	for (uint32_t slice = 0; slice < count; slice++) {
		slots[slice]           = m_clear_predicate_slot;
		m_clear_predicate_slot = (m_clear_predicate_slot + 1) % PredicateSlots;
		uint32_t push[12] {};
		push[0] = static_cast<uint32_t>((lead + slice_size * (first + slice)) / sizeof(uint32_t));
		push[1] = static_cast<uint32_t>(slice_size / sizeof(uint32_t));
		push[2] = slots[slice] * 8;
		push[3] = candidate_count;
		for (uint32_t c = 0; c < 8; c++) {
			push[4 + c] = candidates[c];
		}
		cmd.pushConstants(m_clear_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
		                  sizeof(push), push);
		cmd.dispatch(1, 1, 1);
	}

	vk::MemoryBarrier2 after {};
	after.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask  = vk::PipelineStageFlagBits2::eConditionalRenderingEXT |
	                     vk::PipelineStageFlagBits2::eAllCommands;
	after.dstAccessMask = vk::AccessFlagBits2::eConditionalRenderingReadEXT |
	                      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	dependency.pMemoryBarriers = &after;
	cmd.pipelineBarrier2(dependency);

	{
		std::scoped_lock lock {m_lock};
		auto&            image = m_slot_images[id];
		TrackImage(id);
		const auto& view_desc = desc.view_info;
		for (uint32_t slice = 0; slice < count; slice++) {
			ImageViewInfo view {};
			view.format      = view_desc.format;
			view.type        = vk::ImageViewType::e2D;
			view.base_level  = view_desc.base_level;
			view.base_layer  = image_first + slice;
			view.layer_count = 1;
			view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
			image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
			              vk::AccessFlagBits2::eColorAttachmentWrite |
			                  vk::AccessFlagBits2::eColorAttachmentRead,
			              {}, cmd);
			vk::RenderingAttachmentInfo attachment {};
			attachment.imageView   = image.FindView(view);
			attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
			attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
			attachment.storeOp     = vk::AttachmentStoreOp::eStore;
			const vk::Extent2D extent {std::max(image.info.extent.width >> view.base_level, 1u),
			                           std::max(image.info.extent.height >> view.base_level, 1u)};
			vk::RenderingInfo rendering {};
			rendering.renderArea.extent    = extent;
			rendering.layerCount           = 1;
			rendering.colorAttachmentCount = 1;
			rendering.pColorAttachments    = &attachment;
			cmd.beginRendering(&rendering);
			for (uint32_t c = 0; c < candidate_count; c++) {
				vk::ConditionalRenderingBeginInfoEXT condition {};
				condition.buffer = m_clear_predicates->Handle();
				condition.offset = (slots[slice] * 8 + c) * sizeof(uint32_t);
				cmd.beginConditionalRenderingEXT(&condition);
				vk::ClearAttachment clear {};
				clear.aspectMask      = vk::ImageAspectFlagBits::eColor;
				clear.colorAttachment = 0;
				clear.clearValue      = clears[c];
				vk::ClearRect rect {};
				rect.rect.extent    = extent;
				rect.baseArrayLayer = 0;
				rect.layerCount     = 1;
				cmd.clearAttachments(1, &clear, 1, &rect);
				cmd.endConditionalRenderingEXT();
			}
			cmd.endRendering();
		}
		CommitGpuWrite(image);
		image.clear_meta_tick  = m_buffer_cache.GpuWriteTick(range.address, range.size);
		image.clear_check_tick = m_scheduler.CurrentTick();
	}
	return true;
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& desc,
                                       uint32_t metadata_base_layer) {
	if (desc.info.metadata.kind != ImageMetadataKind::Dcc &&
	    desc.info.metadata.kind != ImageMetadataKind::Cmask) {
		return;
	}
	// Painting the fast clear is on by default. Beast of Reincarnation clears its separate-
	// translucency target (0,0,0,1) with a DCC fast clear on memory the transient pool had just
	// used for another surface; without the paint the composite multiplies the whole scene by a
	// stale alpha of 0 and every 3D frame is black. The paint used to be gated off because it
	// fired on images the GPU had already rewritten; the write-tick check below now skips those.
	// KYTY_DCC_CLEAR=0, or a no_dcc_clear.txt file beside the executable (the launcher spawns the
	// emulator as a child, so an env var set in another shell never reaches it), turns it off.
	static const bool paint_dcc_clear = [] {
		if (const char* value = std::getenv("KYTY_DCC_CLEAR"); value != nullptr) {
			return value[0] != '0';
		}
		FILE* f = std::fopen("no_dcc_clear.txt", "r");
		if (f == nullptr) {
			return true;
		}
		(void)std::fclose(f);
		LOGF("DCC CLEAR PAINT: disabled by no_dcc_clear.txt\n");
		return false;
	}();
	const auto register_only = [&] {
		std::scoped_lock lock {m_lock};
		m_slot_images[id].info.metadata = desc.info.metadata;
		m_surface_metas.erase(desc.info.metadata.range.address);
	};
	// EXPERIMENT (diagnostic): say whether a 1080p RGBA16F target's metadata was rewritten after
	// the image's last GPU write, i.e. whether a fast clear is pending on an aliased image.
	if (desc.type == BindingType::RenderTarget && desc.info.extent.width == 1920 &&
	    desc.info.extent.height == 1080 && desc.view_info.format == vk::Format::eR16G16B16A16Sfloat) {
		const auto dbg_frame = g_dbg_frame.load(std::memory_order_relaxed);
		if (dbg_frame >= 1995 && dbg_frame <= 2000) {
			const auto range      = desc.info.metadata.range;
			const bool meta_gpu   = m_buffer_cache.IsRegionGpuModified(range.address, range.size);
			const auto meta_tick  = m_buffer_cache.GpuWriteTick(range.address, range.size);
			uint64_t   image_tick = 0;
			bool       image_gpu  = false;
			{
				std::scoped_lock lock {m_lock};
				image_tick = m_slot_images[id].gpu_write_tick;
				image_gpu  = m_slot_images[id].IsGpuModified();
			}
			LOGF("CLEAR PROBE: frame=%" PRIu64 " rt=0x%010" PRIx64 " id=%u/%u kind=%d meta=0x%010" PRIx64
			     "+0x%" PRIx64 " meta_gpu=%d meta_tick=%" PRIu64 " image_gpu=%d image_tick=%" PRIu64
			     " clear_reg=%d word=0x%016" PRIx64 "\n",
			     g_dbg_frame.load(std::memory_order_relaxed), desc.info.data.address, id.index,
			     id.generation, static_cast<int>(desc.info.metadata.kind), range.address,
			     range.size, meta_gpu ? 1 : 0, meta_tick, image_gpu ? 1 : 0, image_tick,
			     desc.info.metadata.clear_register_valid ? 1 : 0,
			     static_cast<uint64_t>(desc.info.metadata.clear_word));
		}
	}
	if (!paint_dcc_clear) {
		register_only();
		return;
	}
	// A fast clear belongs to a target acquisition, not to a surface being acquired to be read.
	if (desc.type != BindingType::RenderTarget && desc.type != BindingType::DepthTarget) {
		register_only();
		return;
	}
	// The metadata says "uniformly cleared", which stops being true the moment the GPU writes the
	// surface. Kyty re-reads it on every acquisition, so an image the GPU already owns would be
	// repainted from stale information.
	{
		// A transient pool hands one image to several surfaces, so "GPU-modified" alone does not
		// mean the metadata is stale: the clear is current when the metadata was written after the
		// image's last GPU write.
		const auto meta_tick = m_buffer_cache.GpuWriteTick(desc.info.metadata.range.address,
		                                                   desc.info.metadata.range.size);
		std::scoped_lock lock {m_lock};
		if (m_slot_images[id].IsGpuModified() && meta_tick <= m_slot_images[id].gpu_write_tick) {
			m_slot_images[id].info.metadata = desc.info.metadata;
			m_surface_metas.erase(desc.info.metadata.range.address);
			return;
		}
	}
	const auto range = desc.info.metadata.range;
	{
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// Native color metadata must not retain a reused HTile/CMask/FMask clear flag.
		m_surface_metas.erase(range.address);
		if (range.size == 0 || desc.info.resources.levels != 1 || image.info.resources.levels != 1) {
			return;
		}
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks.
	constexpr uint64_t MetadataBlockSize = 0x1000;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	// Finish native metadata writes before reading backing bytes. This can submit the scheduler,
	// so discovery runs before final draw uploads and never holds the texture lock across it.
	if (m_buffer_cache.IsRegionGpuModified(range.address, range.size) &&
	    PaintColorClearOnGpu(id, desc, first, image_first, count, layers)) {
		return;
	}
	if (m_buffer_cache.IsRegionGpuModified(range.address, range.size)) {
		// Reading GPU-written metadata flushes and waits for the GPU. Skip it when this image has
		// already evaluated this version of the metadata in an earlier submission: nothing has
		// rewritten the keys since, so the answer cannot have changed.
		const auto meta_tick = m_buffer_cache.GpuWriteTick(range.address, range.size);
		{
			std::scoped_lock lock {m_lock};
			const auto&      image = m_slot_images[id];
			if (meta_tick != 0 && meta_tick <= image.clear_meta_tick &&
			    image.clear_check_tick < m_scheduler.CurrentTick()) {
				return;
			}
		}
		m_buffer_cache.ReadMemory(range.address, range.size, false);
		std::scoped_lock lock {m_lock};
		auto&            image = m_slot_images[id];
		image.clear_meta_tick  = meta_tick;
		image.clear_check_tick = m_scheduler.CurrentTick();
	}
	const auto slice_size = range.size / layers;
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		uint8_t code = 0;
		if (!LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			EXIT("TextureCache: failed to read color metadata backing\n");
		}
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		std::vector<uint8_t> bytes(slice_size);
		if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
			EXIT("TextureCache: failed to read color metadata slice\n");
		}
		if (!std::all_of(bytes.begin(), bytes.end(), [code](uint8_t byte) { return byte == code; })) {
			continue;
		}
		{
			std::scoped_lock lock {m_lock};
			// Debug: a fast clear paints a whole slice. If the colour is at or above white this
			// is a candidate for the glowing blocks, so record what colour lands on which image.
			const auto& image = m_slot_images[id];
			if (image.info.extent.width == 1920 && image.info.extent.height == 1080) {
				LOGF("DCC CLEAR: frame=%" PRIu64 " seq=%" PRIu64 " image=0x%016" PRIx64
				     " %ux%u fmt=%u slice=%u code=0x%02x rgba=%g,%g,%g,%g\n",
				     g_dbg_frame.load(std::memory_order_relaxed),
				     g_dbg_seq.fetch_add(1, std::memory_order_relaxed), image.info.data.address,
				     image.info.extent.width, image.info.extent.height,
				     static_cast<uint32_t>(image.backing.format), image_first + slice, code,
				     static_cast<double>(clear.color.float32[0]),
				     static_cast<double>(clear.color.float32[1]),
				     static_cast<double>(clear.color.float32[2]),
				     static_cast<double>(clear.color.float32[3]));
			}
			// Debug: the 1080p filter above hides the small surfaces, and the glowing blocks are
			// block-shaped, so report every distinct (image, extent, format, colour) once. That
			// is what says whether anything is being fast-cleared to white.
			{
				static std::mutex dbg_mutex;
				static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>>
				     dbg_seen;
				bool first = false;
				{
					std::scoped_lock dbg_lock {dbg_mutex};
					first = dbg_seen
					            .emplace(image.info.data.address, image.info.extent.width,
					                     image.info.extent.height,
					                     static_cast<uint32_t>(image.backing.format), code)
					            .second;
				}
				if (first) {
					LOGF("DCC PAINT: binding=%d image=0x%016" PRIx64 " %ux%u fmt=%u code=0x%02x"
					     " rgba=%g,%g,%g,%g\n",
					     static_cast<int>(desc.type), image.info.data.address,
					     image.info.extent.width, image.info.extent.height,
					     static_cast<uint32_t>(image.backing.format), code,
					     static_cast<double>(clear.color.float32[0]),
					     static_cast<double>(clear.color.float32[1]),
					     static_cast<double>(clear.color.float32[2]),
					     static_cast<double>(clear.color.float32[3]));
				}
			}
			ClearImage(m_scheduler.Current(), id, view.format,
			           {vk::ImageAspectFlagBits::eColor, view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		// Native expanded keys own consumption. Existing buffer tracking publishes this CPU
		// write to future GPU readers; FillBuffer can fault and must run outside the texture lock.
		if (desc.type != BindingType::VideoOut) {
			m_buffer_cache.FillBuffer(address, slice_size, UINT32_MAX, false);
			// Debug: the publish is what stops this surface being re-read as "still fast
			// cleared" on the next acquisition. If it does not land in guest memory the same
			// clear repaints every frame forever, which is exactly what one image did 1392 times.
			// Read the byte back through the same path the next frame will use and report a
			// mismatch.
			{
				uint8_t    after = 0;
				const bool read_ok =
				    LibKernel::Memory::TryReadBacking(address, &after, sizeof(after));
				if (!read_ok || after != 0xffu) {
					static std::mutex         dbg_mutex;
					static std::set<uint64_t> dbg_seen;
					bool                      first = false;
					{
						std::scoped_lock dbg_lock {dbg_mutex};
						first = dbg_seen.emplace(address).second;
					}
					if (first) {
						LOGF("DCC PUBLISH FAILED: metadata=0x%016" PRIx64 " size=0x%" PRIx64
						     " wrote=0xff read_back=0x%02x read_ok=%d (this surface will re-clear"
						     " every frame)\n",
						     address, slice_size, after, read_ok ? 1 : 0);
					}
				}
			}
			// That publish writes the guest mapping directly so page faults invalidate stale
			// readers. The metadata lives inside this surface's own padded guest range, so the
			// faults also condemn the image the clear just wrote: InvalidateCpuAliases sets
			// cpu_dirty on it, and the next bind uploads guest memory over the clear and
			// everything drawn after it. Measured on Beast of Reincarnation: 40636 such
			// invalidations in 542 frames, every one a single byte discarding an 8.44 MiB
			// GPU-owned image, all of them inside 80 sequence numbers of a clear. The clear is
			// the authoritative content at this point, so re-assert the image's ownership of it.
			// Aliases of the metadata range keep their invalidation, which is what they want.
			MarkGpuWritten(id);
		}
	}
}

void TextureCache::RefreshImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		const auto hash = image.HashGuestEdges();
		if (image.NeedsMaybeCpuHash()) {
			image.SetMaybeCpuHash(hash);
			return;
		}
		(void)image.ResolveMaybeCpuHash(hash);
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	// Debug: this is the only place a live image's contents are replaced from guest memory, and
	// it is what erases a compute result before the compositor samples it. Name which of the two
	// flags forced it, so a black or striped frame can be traced to the write that caused it.
	if (DebugGfxTraceEnabled() && image.info.extent.width == 1920 &&
	    (image.info.extent.height == 1080 || image.info.extent.height == 1620)) {
		LOGF("REFRESH UPLOAD: frame=%" PRIu64 " seq=%" PRIu64 " addr=0x%016" PRIx64
		     " size=0x%" PRIx64 " %ux%u fmt=%u buffer_modified=%d cpu_dirty=%d maybe_cpu=%d"
		     " gpu_modified=%d\n",
		     g_dbg_frame.load(std::memory_order_relaxed),
		     g_dbg_seq.fetch_add(1, std::memory_order_relaxed), image.info.data.address,
		     image.info.data.size, image.info.extent.width, image.info.extent.height,
		     static_cast<uint32_t>(image.backing.format), image.IsBufferModified() ? 1 : 0,
		     image.IsDefinitelyCpuDirty() ? 1 : 0, image.IsMaybeCpuDirty() ? 1 : 0,
		     image.IsGpuModified() ? 1 : 0);
	}
	InitializeImage(id);
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	record.depth_id = depth_id;
	return association;
}

// Binding this memory as something other than a stencil plane proves the guest has repurposed it,
// so the association no longer describes the surface. Dropping it is safe because every depth
// target bind re-associates its stencil range.
void TextureCache::DropStencilAssociation(ImageId id) {
	std::scoped_lock lock {m_lock};
	if (auto* image = m_slot_images.try_get(id); image != nullptr) {
		image->depth_id = {};
	}
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	if (desc.info.data.Empty()) {
		std::scoped_lock lock {m_lock};
		return GetNullImage(desc);
	}
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		std::scoped_lock lock {m_lock};
		const auto       candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);

		uint32_t dbg_same_backing = 0;
		ImageId  gpu_owner {};
		for (const auto id: candidates) {
			const auto& image = m_slot_images[id];
			if (SameBacking(image.info, desc.info, exact_format)) {
				dbg_same_backing++;
				result = id;
				if (image.IsGpuModified()) {
					gpu_owner = id;
				}
			}
		}
		// Several live images can share identical backing. Candidate order then decides the
		// winner, which is how a pass samples an image that never received the pixels drawn
		// at that address. Whoever holds GPU output owns those bytes; prefer it.
		if (gpu_owner) {
			result = gpu_owner;
		}
		// Debug: more than one live image with identical backing means the winner here is
		// candidate order, not recency -- exactly how a pass can be handed the wrong image.
		if (dbg_same_backing > 1) {
			static std::atomic_uint64_t dbg_amb {0};
			const auto n = dbg_amb.fetch_add(1, std::memory_order_relaxed);
			if (n < 40 || (n % 500) == 0) {
				LOGF("TexCache AMBIGUOUS #%" PRIu64 ": addr=0x%016" PRIx64 " size=0x%" PRIx64
				     " type=%d matches=%" PRIu32 " chosen=%u\n",
				     n, desc.info.data.address, desc.info.data.size,
				     static_cast<int>(desc.type), dbg_same_backing,
				     static_cast<uint32_t>(result.index));
				for (const auto id: candidates) {
					const auto& image = m_slot_images[id];
					if (SameBacking(image.info, desc.info, exact_format)) {
						LOGF("    cand id=%u tick=%" PRIu64 " gpu_mod=%d rt=%d\n",
						     static_cast<uint32_t>(id.index),
						     static_cast<uint64_t>(image.tick_accessed_last),
						     image.IsGpuModified() ? 1 : 0, image.usage.render_target ? 1 : 0);
					}
				}
			}
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				view_mip                = -1;
				view_layer              = -1;
				const auto& merged_info = result ? m_slot_images[result].info : desc.info;
				const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
					// Debug: a bind merged onto an image that starts at a different guest address.
					// Two surfaces sharing one image overwrite each other.
					const auto& merged = m_slot_images[result];
					if (merged.info.data.address != desc.info.data.address &&
					    desc.info.extent.width >= 640) {
						static std::atomic_uint64_t dbg_alias {0};
						const auto n = dbg_alias.fetch_add(1, std::memory_order_relaxed);
						if (n < 40 || (n % 500) == 0) {
							LOGF("RT ALIAS #%" PRIu64 ": want=0x%016" PRIx64 " %ux%u fmt=%u"
							     " -> image=0x%016" PRIx64 " %ux%u fmt=%u mip=%d layer=%d type=%d\n",
							     n, desc.info.data.address, desc.info.extent.width,
							     desc.info.extent.height, static_cast<uint32_t>(desc.info.pixel_format),
							     merged.info.data.address, merged.info.extent.width,
							     merged.info.extent.height,
							     static_cast<uint32_t>(merged.info.pixel_format), view_mip, view_layer,
							     static_cast<int>(desc.type));
						}
					}
				}
			}
		}

		if (result) {
			auto& resolved = m_slot_images[result];
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				FreeImage(result);
				result = {};
			}
		}
		if (!result) {
			result         = InsertImage(desc.info);
			auto& inserted = m_slot_images[result];
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified();
			}
		}
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	MaterializeColorClear(result, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[result];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
	return result;
}

void TextureCache::UpdateImage(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = m_slot_images.try_get(owner->depth_id);
		}
		if (owner == nullptr || (ensure_valid && !SafeToDownload(*owner))) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected && ensure_valid) {
		const auto owner = m_slot_images.try_get(selected);
		if (owner != nullptr && owner->depth_id) {
			selected = owner->depth_id;
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	if (!image.info.data.Empty()) {
		if (!image.registered || image.depth_id || image.binding.needs_rebind) {
			EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
		}
	}
	if (desc.type == BindingType::Storage) {
		image.MarkGpuModified();
	}
	if (!image.info.data.Empty()) {
		RefreshImage(id);
		if (image.info.HasStencil() &&
		    desc.info.data.address >= image.info.stencil.address &&
		    desc.info.data.End() <= image.info.stencil.End()) {
			for (const auto stencil_id:
			     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
				const auto* stencil = m_slot_images.try_get(stencil_id);
				if (stencil != nullptr && stencil->depth_id == id &&
				    stencil->info.data == image.info.stencil) {
					RefreshImage(stencil_id);
					break;
				}
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			if (!image.info.data.Empty()) {
				CommitGpuWrite(image);
			}
			TrackImageDownload(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.render_target = true;
	// Debug: enumerate the distinct colour targets this run writes.
	{
		static std::set<std::pair<uint64_t, uint32_t>> dbg_seen;
		const std::pair<uint64_t, uint32_t>            key {
		    image.info.data.address, static_cast<uint32_t>(image.backing.format)};
		if (dbg_seen.insert(key).second) {
			LOGF("RT ENUM #%zu: addr=0x%016" PRIx64 " %ux%u fmt=%u layers=%u\n",
			     dbg_seen.size() - 1, image.info.data.address, image.info.extent.width,
			     image.info.extent.height, static_cast<uint32_t>(image.backing.format),
			     image.backing.layers);
			if (image.info.extent.width == 1920 && image.info.extent.height == 1080) {
				DebugRegisterRenderTargetId(id.index, id.generation);
				if (image.backing.format == vk::Format::eR16G16B16A16Sfloat) {
					DebugRecordHdrWritten(image.info.data.address);
				}
			}
		}
	}
	if (DebugGfxTraceEnabled() && image.info.extent.width == 1920 &&
	    image.info.extent.height == 1080) {
		LOGF("PRODUCE: frame=%" PRIu64 " seq=%" PRIu64 " addr=0x%016" PRIx64 " size=0x%" PRIx64
		     " %ux%u fmt=%u guest_fmt=%u mips=%u layers=%u\n",
		     g_dbg_frame.load(std::memory_order_relaxed),
		     g_dbg_seq.fetch_add(1, std::memory_order_relaxed), image.info.data.address,
		     image.info.data.size, image.info.extent.width, image.info.extent.height,
		     static_cast<uint32_t>(image.backing.format),
		     static_cast<uint32_t>(image.info.guest_format), image.info.resources.levels,
		     image.info.resources.layers);
	}
	// Refresh the address->id map on every acquisition, not just the first: the cache recycles
	// slots, so a generation captured once is dead by present time.
	if (image.info.extent.width == 1920 && image.info.extent.height == 1080) {
		DebugRegisterRenderTargetAddress(image.info.data.address,
		                                 static_cast<uint32_t>(image.backing.format), id.index,
		                                 id.generation);
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	TrackImageDownload(id, image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.depth_target = true;
	image.info.stencil = desc.info.stencil;
	image.info.metadata = desc.info.metadata;
	if (desc.info.HasMetadata()) {
		m_surface_metas.emplace(desc.info.metadata.range.address,
		                        MetaDataInfo {.type       = MetaDataInfo::Type::HTile,
		                                      .clear_mask = image.info.htile_clear_mask});
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	if (desc.info.HasStencil()) {
		RefreshImage(AssociateStencil(id, desc.info.stencil));
	}
	return image.FindView(desc.view_info);
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.gpu_write_tick = m_scheduler.CurrentTick();
	image.MarkGpuModified();
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	std::scoped_lock     lock {m_lock};
	ImageId              selected {};
	vk::ImageAspectFlags aspect {};
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate    = vk::ImageAspectFlagBits::eStencil;
			candidate_id = owner->depth_id;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           owner->info.data.size == size) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && selected != candidate_id) {
			return false;
		}
		selected = candidate_id;
		aspect   = candidate;
	}
	if (!selected) {
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (PackedColorClearNeedsHighWord(image.info.pixel_format) ||
		    !DecodePackedColorClear(image.info.pixel_format, packed_clear, 0, clear.color)) {
			return false;
		}
	} else {
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, 0, image.info.resources.levels, 0, image.info.TransferLayers()}, clear);
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	auto& image = m_slot_images[id];
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		} else {
			TrackImage(stencil_id);
			CommitGpuWrite(m_slot_images[stencil_id]);
		}
	}
	command.EndRendering();
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		ImageViewInfo view {};
		view.format = format;
		view.type   = range.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.base_level  = range.baseMipLevel;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;
		view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
		image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		              vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		vk::RenderingAttachmentInfo attachment {};
		attachment.imageView   = image.FindView(view);
		attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.loadOp      = vk::AttachmentLoadOp::eClear;
		attachment.storeOp     = vk::AttachmentStoreOp::eStore;
		attachment.clearValue  = clear;
		vk::RenderingInfo rendering {};
		rendering.renderArea.extent = {
		    std::max(image.info.extent.width >> range.baseMipLevel, 1u),
		    std::max(image.info.extent.height >> range.baseMipLevel, 1u)};
		rendering.layerCount           = range.layerCount;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &attachment;
		command.Handle().beginRendering(&rendering);
		command.Handle().endRendering();
		CommitGpuWrite(image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	CommitGpuWrite(image);
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	std::scoped_lock lock {m_lock};
	InvalidateCpuAliases(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.data.size / layers;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	auto copies = BuildDepthCopies(info, full_slice_size, vk::ImageAspectFlagBits::eDepth);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			for (auto& copy: copies) {
				copy.bufferOffset += destination_offset;
			}
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
			return;
		}
		const auto tiles = BuildDepthTiles(info);
		m_tiler.TileImage(image, copies, destination.Handle(), destination_offset, info.data.size,
		                  info.data.size, tiles);
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	for (uint32_t layer = 0; layer < layers; layer++) {
		copies[layer].bufferOffset = transfer_slice * layer;
	}
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto       guest_linear = tiled ? m_tiler.GetScratchBuffer(info.data.size, host_linear.buffer)
	                                : TileManager::Result {destination.Handle(), destination_offset,
	                                                       destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (!tiled) {
		return;
	}
	const auto tiles = BuildDepthTiles(info);
	m_tiler.Tile(guest_linear.buffer, guest_linear.offset, info.data.size, destination.Handle(),
	             destination_offset, info.data.size, tiles);
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                     uint64_t destination_size, ImageDownload transfer) {
	if (!transfer.valid) {
		EXIT("TextureCache: invalid image download transfer\n");
	}
	if (transfer.depth_target) {
		if (destination_size != image.info.data.size) {
			EXIT("TextureCache: partial depth image download is unsupported\n");
		}
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	auto&      texture   = transfer.texture;
	const auto transform = texture.swap_bgra16 ? TileManager::ColorTransform::SwapBgra16
	                                           : TileManager::ColorTransform::None;
	if (texture.tiles.empty()) {
		if (transform == TileManager::ColorTransform::SwapBgra16) {
			auto linear = m_tiler.GetScratchBuffer(destination_size);
			image.Download(texture.regions, linear.buffer, 0, linear.size);
			m_tiler.SwapBgra16(linear,
			                   {destination.Handle(), destination_offset, destination_size});
			return;
		}
		for (auto& copy: texture.regions) {
			copy.bufferOffset += destination_offset;
		}
		image.Download(texture.regions, destination.Handle(), destination_offset, destination_size);
		return;
	}

	m_tiler.TileImage(image, texture.regions, destination.Handle(), destination_offset,
	                  destination_size, texture.LinearSize(), texture.tiles, transform);
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto selected = m_texture_cache.FindImageFromRange(vaddr, size);
	if (!selected) {
		return false;
	}

	std::scoped_lock lock {m_texture_cache.m_lock};
	auto& image = m_texture_cache.m_slot_images[selected];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!m_texture_cache.SafeToDownload(image)) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (image.info.IsVolume()) {
		// Volume mips contain strided block slices, so a mip's linear span cannot prove that
		// every retained slice fits. Keep volume synchronization whole-image only.
		if (!buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
			return false;
		}
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	auto transfer = m_texture_cache.BuildDownload(image);
	if (!transfer.valid) {
		return false;
	}
	if (transfer.depth_target && copy_size != image.info.data.size) {
		return false;
	}
	if (!transfer.depth_target && levels < image.info.resources.levels) {
		auto& texture = transfer.texture;
		std::erase_if(texture.regions, [levels](const vk::BufferImageCopy& region) {
			return region.imageSubresource.mipLevel >= levels;
		});
		if (texture.regions.empty()) {
			return false;
		}
		if (!texture.tiles.empty()) {
			texture.tiles.clear();
			if (!TextureBuildGpuTileInfos(copy_size, texture.regions, texture.layout, levels,
			                              texture.tiles)) {
				return false;
			}
		}
	}
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, std::move(transfer));
	return true;
}

bool TextureCache::DownloadImageMemory(ImageId id) {
	auto& image = m_slot_images[id];
	// Debug: the image -> guest memory direction. A later UploadImage of the same guest range can
	// only see GPU results if this ran first and covered the bytes. Logged with the reason when
	// it is skipped, keyed like UPLOAD so the two correlate.
	const auto dbg_report = [&](const char* outcome) {
		static std::mutex                                                           dbg_mutex;
		static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, std::string>> dbg_seen;
		const std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, std::string> key {
		    image.info.data.address, image.info.extent.width, image.info.extent.height,
		    static_cast<uint32_t>(image.backing.format), outcome};
		bool first = false;
		{
			std::scoped_lock lock {dbg_mutex};
			first = dbg_seen.insert(key).second;
		}
		if (first) {
			LOGF("DOWNLOAD: image->guest addr=0x%016" PRIx64 " %ux%u fmt=%u"
			     " range=0x%016" PRIx64 "-0x%016" PRIx64 " size=0x%" PRIx64 " outcome=%s\n",
			     image.info.data.address, image.info.extent.width, image.info.extent.height,
			     static_cast<uint32_t>(image.backing.format), image.info.data.address,
			     image.info.data.address + image.info.data.size, image.info.data.size, outcome);
		}
	};
	if (image.depth_id) {
		dbg_report("skipped:depth");
		return false;
	}
	auto transfer = BuildDownload(image);
	if (!transfer.valid) {
		dbg_report("skipped:invalid-transfer");
		return false;
	}
	if (!SafeToDownload(image)) {
		dbg_report("skipped:image-unsafe");
		return false;
	}
	// EXPERIMENT: flush instead of refusing. HasGpuDirtyBytes is an Intersects test, so a single
	// pending buffer write anywhere in the range vetoes the whole readback -- measured at 4 bytes
	// blocking an 8.7 MiB image, and three 1080p targets blocked with 0.6% of their range dirty.
	// Draining those writes into guest memory first preserves the coherency invariant rather than
	// overwriting them, which is the same order MaterializeDccClear already uses.
	{
		const auto image_range = image.info.data;
		if (m_buffer_cache.IsRegionGpuModified(image_range.address, image_range.size)) {
			m_buffer_cache.ReadMemory(image_range.address, image_range.size, false);
			// Diagnostic: did the flush actually drain the condition that caused the refusal?
			if (m_buffer_cache.IsRegionGpuModified(image_range.address, image_range.size)) {
				dbg_report("skipped:still-dirty-after-flush");
				return false;
			}
			dbg_report("performed:after-flush");
		} else {
			dbg_report("performed");
		}
	}
	const auto range    = image.info.data;
	auto&      download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] =
	    download.Map(range.size, std::max<uint64_t>(image.info.bytes_per_block, 4));
	if (mapped == nullptr) {
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) {
		dbg_report("failed:read-backing");
		return false;
	}
	download.Flush(offset, range.size);

	DownloadImage(image, download, offset, range.size, std::move(transfer));
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering();
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	m_scheduler.DeferPriorityOperation([&download, range, mapped, offset] {
		download.Invalidate(offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
	});
	return true;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (!image.Overlaps(address, size)) {
			continue;
		}
		// Debug: a buffer write of any size surrenders the whole image's GPU ownership here. Log
		// the size of the write against the size of the image so a 4-byte write discarding 8 MiB
		// of compute output is visible rather than inferred.
		if (DebugGfxTraceEnabled() && image.info.extent.width == 1920 &&
		    image.info.extent.height == 1080) {
			const auto overlap_start = std::max(address, image.info.data.address);
			const auto overlap_end =
			    std::min(address + size, image.info.data.address + image.info.data.size);
			LOGF("IMAGE INVALIDATE: frame=%" PRIu64 " seq=%" PRIu64 " image=0x%016" PRIx64
			     " size=0x%" PRIx64 " %ux%u fmt=%u by_buffer=0x%016" PRIx64 " size=0x%" PRIx64
			     " overlap=0x%" PRIx64 " was_gpu_modified=%d\n",
			     g_dbg_frame.load(std::memory_order_relaxed),
			     g_dbg_seq.fetch_add(1, std::memory_order_relaxed), image.info.data.address,
			     image.info.data.size, image.info.extent.width, image.info.extent.height,
			     static_cast<uint32_t>(image.backing.format), address, size,
			     overlap_end > overlap_start ? overlap_end - overlap_start : 0,
			     image.IsGpuModified() ? 1 : 0);
		}
		// A buffer write that covers only part of a GPU-owned image does not make the rest of it
		// stale, but surrendering ownership here discards all of it and the next bind re-uploads
		// the whole surface from guest memory. Measured on Beast of Reincarnation: a 4-byte texel
		// buffer write at an image's base address discarded 8.44 MiB of compute output 736 times,
		// once per frame, and 4 KiB writes inside 1080p targets did the same 1047 times each.
		// Keep ownership unless the write covers the whole image.
		//
		// This is narrower than the write deserves: strictly, the overlapping bytes are stale and
		// should be re-uploaded on their own. Doing that needs per-image dirty ranges and a
		// partial detile path, which the tiler cannot do yet, so the overlap is currently held
		// rather than refreshed. KYTY_INVALIDATE_WHOLE_IMAGE restores the old behaviour for
		// anyone bisecting a surface that goes stale.
		static const bool invalidate_whole_image =
		    std::getenv("KYTY_INVALIDATE_WHOLE_IMAGE") != nullptr;
		if (!invalidate_whole_image && image.IsGpuModified()) {
			const auto covers_whole_image =
			    address <= image.info.data.address &&
			    address + size >= image.info.data.address + image.info.data.size;
			if (!covers_whole_image) {
				continue;
			}
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		image.MarkBufferModified();
	}
}

bool TextureCache::IsRegionGpuModified(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return false;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, false)) {
		const auto& image = m_slot_images[id];
		// PPSA17168: S_LOAD_DWORD reads shader data at an address overlapping an old
		// render target whose memory the CPU has reused. The cached image still retains
		// its earlier GPU-modified flag.
		if (!image.depth_id && image.IsGpuModified() && !image.IsDefinitelyCpuDirty()) {
			return true;
		}
	}
	return false;
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->Overlaps(address, size)) {
			// Debug: the other half of the "who erased the compute result" question. With the
			// buffer route suppressed, cpu_dirty alone still forces uploads, and this is where it
			// is set. Log the CPU write against the image it condemns.
			if (owner->info.extent.width == 1920 && owner->info.extent.height == 1080) {
				const auto overlap_start = std::max(address, owner->info.data.address);
				const auto overlap_end =
				    std::min(address + size, owner->info.data.address + owner->info.data.size);
				LOGF("CPU INVALIDATE: frame=%" PRIu64 " seq=%" PRIu64 " image=0x%016" PRIx64
				     " size=0x%" PRIx64 " %ux%u fmt=%u by_cpu=0x%016" PRIx64 " size=0x%" PRIx64
				     " overlap=0x%" PRIx64 " was_gpu_modified=%d\n",
				     g_dbg_frame.load(std::memory_order_relaxed),
				     g_dbg_seq.fetch_add(1, std::memory_order_relaxed), owner->info.data.address,
				     owner->info.data.size, owner->info.extent.width, owner->info.extent.height,
				     static_cast<uint32_t>(owner->backing.format), address, size,
				     overlap_end > overlap_start ? overlap_end - overlap_start : 0,
				     owner->IsGpuModified() ? 1 : 0);
			}
			owner->InvalidateCpuWrite(address, size);
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

bool TextureCache::IsMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	return found != m_surface_metas.end();
}

Image* TextureCache::DebugTryGetImage(uint32_t id_index, uint32_t id_generation) {
	std::scoped_lock lock {m_lock};
	return m_slot_images.try_get(ImageId {id_index, id_generation});
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	if (slice >= MetaSliceBits) {
		static std::atomic<bool> warned {false};
		if (!warned.exchange(true)) {
			LOGF("TextureCache: metadata slice %" PRIu32 " of 0x%016" PRIx64
			     " is beyond the %u tracked slices; its clear state is not tracked"
			     " (further warnings suppressed)\n",
			     slice, address, MetaSliceBits);
		}
		return false;
	}
	return (found->second.clear_mask & (UINT64_C(1) << slice)) != 0;
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask = UINT64_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	if (slice >= MetaSliceBits) {
		static std::atomic<bool> warned {false};
		if (!warned.exchange(true)) {
			LOGF("TextureCache: metadata slice %" PRIu32 " of 0x%016" PRIx64
			     " is beyond the %u tracked slices; its clear state is not tracked"
			     " (further warnings suppressed)\n",
			     slice, address, MetaSliceBits);
		}
		return false;
	}
	if (is_clear) {
		found->second.clear_mask |= UINT64_C(1) << slice;
	} else {
		found->second.clear_mask &= ~(UINT64_C(1) << slice);
	}
	return true;
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
	}
}

void TextureCache::RunGarbageCollector() {
	std::scoped_lock lock {m_lock};
	const uint64_t   tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = m_total_used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && m_total_used_memory >= m_critical_gc_memory;
		const uint64_t age       = std::min<uint64_t>(aggressive ? 160 : pressured ? 80 : 16, tick);
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
		std::vector<ImageId> candidates;
		candidates.reserve(deletions);
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first.
		m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
			candidates.push_back(id);
			return candidates.size() == deletions;
		});
		for (const auto id: candidates) {
			if (deletions == 0) {
				break;
			}
			--deletions;
			auto owner = m_slot_images.try_get(id);
			if (owner == nullptr || !owner->registered || owner->depth_id) {
				continue;
			}
			if (owner->IsGpuModified()) {
				const bool safe = SafeToDownload(*owner);
				if (safe && owner->info.IsTiled()) {
					continue;
				}
				if (safe && !pressured) {
					continue;
				}
				if (safe && !DownloadImageMemory(id)) {
					continue;
				}
			}
			FreeImage(id);
			if (m_total_used_memory < m_critical_gc_memory && aggressive) {
				deletions >>= 2;
				aggressive = false;
			}
			if (m_total_used_memory < m_pressure_gc_memory && pressured) {
				deletions >>= 1;
				pressured = false;
			}
		}
	};
	collect(false);
	if (m_total_used_memory >= m_critical_gc_memory) {
		collect(true);
	}
}

void TextureCache::ProcessDownloadImages() {
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
