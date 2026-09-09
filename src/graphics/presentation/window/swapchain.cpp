#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

std::atomic<uint32_t> g_gpu_dump_requested {0};

void RequestGpuImageDump() {
	g_gpu_dump_requested.store(1, std::memory_order_release);
}

namespace {

[[nodiscard]] uint8_t Unorm10To8(uint32_t value) {
	return static_cast<uint8_t>((value * 255u + 511u) / 1023u);
}

[[nodiscard]] uint8_t Saturate8(float value) {
	if (value <= 0.0f) {
		return 0;
	}
	if (value >= 1.0f) {
		return 255;
	}
	return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

[[nodiscard]] float DecodeUnsignedFloat(uint32_t packed, uint32_t mantissa_bits,
                                        uint32_t exponent_bits) {
	const uint32_t mantissa_mask = (1u << mantissa_bits) - 1u;
	const uint32_t exponent_mask = (1u << exponent_bits) - 1u;
	const auto     mantissa      = packed & mantissa_mask;
	const auto     exponent      = (packed >> mantissa_bits) & exponent_mask;
	const int      bias          = static_cast<int>(exponent_mask >> 1u) - 1;
	if (exponent == 0) {
		return std::ldexp(static_cast<float>(mantissa), 1 - bias - static_cast<int>(mantissa_bits));
	}
	return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits),
	                  static_cast<int>(exponent) - bias);
}

void WriteBmpBgra(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  const std::vector<uint8_t>& bgra) {
	const uint32_t pixel_bytes = width * height * 4u;
	const uint32_t file_size   = 54u + pixel_bytes;
	std::ofstream  out(path, std::ios::binary);
	if (!out) {
		LOGF("VideoOut dump: failed to write %s\n", path.string().c_str());
		return;
	}
	const uint8_t header[54] = {
	    'B',
	    'M',
	    static_cast<uint8_t>(file_size),
	    static_cast<uint8_t>(file_size >> 8),
	    static_cast<uint8_t>(file_size >> 16),
	    static_cast<uint8_t>(file_size >> 24),
	    0,
	    0,
	    0,
	    0,
	    54,
	    0,
	    0,
	    0,
	    40,
	    0,
	    0,
	    0,
	    static_cast<uint8_t>(width),
	    static_cast<uint8_t>(width >> 8),
	    static_cast<uint8_t>(width >> 16),
	    static_cast<uint8_t>(width >> 24),
	    static_cast<uint8_t>(height),
	    static_cast<uint8_t>(height >> 8),
	    static_cast<uint8_t>(height >> 16),
	    static_cast<uint8_t>(height >> 24),
	    1,
	    0,
	    32,
	    0,
	};
	out.write(reinterpret_cast<const char*>(header), sizeof(header));
	// BMP is bottom-up.
	for (uint32_t y = height; y-- > 0;) {
		out.write(reinterpret_cast<const char*>(bgra.data() + static_cast<size_t>(y) * width * 4u),
		          static_cast<std::streamsize>(width * 4u));
	}
}

[[nodiscard]] float HalfToFloat(uint16_t value) {
	const uint32_t sign     = static_cast<uint32_t>(value >> 15) << 31;
	const uint32_t exponent = (value >> 10) & 0x1fu;
	const uint32_t mantissa = value & 0x3ffu;
	uint32_t       bits     = 0;
	if (exponent == 0) {
		if (mantissa == 0) {
			bits = sign;
		} else {
			uint32_t m = mantissa;
			uint32_t e = 1;
			while ((m & 0x400u) == 0) {
				m <<= 1;
				e--;
			}
			m &= 0x3ffu;
			bits = sign | ((e + 127u - 15u) << 23) | (m << 13);
		}
	} else if (exponent == 31) {
		bits = sign | 0x7f800000u | (mantissa << 13);
	} else {
		bits = sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
	}
	float decoded = 0.0f;
	std::memcpy(&decoded, &bits, sizeof(decoded));
	return decoded;
}

void ConvertPackedToBgra(vk::Format format, uint32_t width, uint32_t height,
                         const uint8_t* packed, std::vector<uint8_t>& bgra, uint64_t& nonzero,
                         uint32_t& max_channel) {
	bgra.resize(static_cast<size_t>(width) * height * 4u);
	nonzero     = 0;
	max_channel = 0;
	const uint32_t count = width * height;
	auto           put   = [&](size_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        bgra[i * 4 + 0] = b;
        bgra[i * 4 + 1] = g;
        bgra[i * 4 + 2] = r;
        bgra[i * 4 + 3] = a;
        const uint8_t peak = std::max({r, g, b});
        max_channel        = std::max(max_channel, static_cast<uint32_t>(peak));
        if (r | g | b) {
            nonzero++;
        }
	};
	const auto* words = reinterpret_cast<const uint32_t*>(packed);
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
			for (uint32_t i = 0; i < count; i++) {
				put(i, packed[i * 4 + 0], packed[i * 4 + 1], packed[i * 4 + 2], packed[i * 4 + 3]);
			}
			return;
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
			for (uint32_t i = 0; i < count; i++) {
				put(i, packed[i * 4 + 2], packed[i * 4 + 1], packed[i * 4 + 0], packed[i * 4 + 3]);
			}
			return;
		case vk::Format::eA2R10G10B10UnormPack32:
			for (uint32_t i = 0; i < count; i++) {
				const uint32_t p = words[i];
				put(i, Unorm10To8((p >> 20) & 0x3ffu), Unorm10To8((p >> 10) & 0x3ffu),
				    Unorm10To8(p & 0x3ffu), static_cast<uint8_t>(((p >> 30) & 0x3u) * 85u));
			}
			return;
		case vk::Format::eA2B10G10R10UnormPack32:
			for (uint32_t i = 0; i < count; i++) {
				const uint32_t p = words[i];
				put(i, Unorm10To8(p & 0x3ffu), Unorm10To8((p >> 10) & 0x3ffu),
				    Unorm10To8((p >> 20) & 0x3ffu),
				    static_cast<uint8_t>(((p >> 30) & 0x3u) * 85u));
			}
			return;
		case vk::Format::eB10G11R11UfloatPack32:
			for (uint32_t i = 0; i < count; i++) {
				const uint32_t p = words[i];
				const float    r = DecodeUnsignedFloat(p & 0x7ffu, 6, 5);
				const float    g = DecodeUnsignedFloat((p >> 11) & 0x7ffu, 6, 5);
				const float    b = DecodeUnsignedFloat((p >> 22) & 0x3ffu, 5, 5);
				put(i, Saturate8(r), Saturate8(g), Saturate8(b), 255);
			}
			return;
		case vk::Format::eR16G16B16A16Sfloat: {
			const auto* halfs = reinterpret_cast<const uint16_t*>(packed);
			for (uint32_t i = 0; i < count; i++) {
				put(i, Saturate8(HalfToFloat(halfs[i * 4 + 0])),
				    Saturate8(HalfToFloat(halfs[i * 4 + 1])),
				    Saturate8(HalfToFloat(halfs[i * 4 + 2])),
				    Saturate8(HalfToFloat(halfs[i * 4 + 3])));
			}
			return;
		}
		case vk::Format::eR16G16B16A16Unorm: {
			const auto* halfs = reinterpret_cast<const uint16_t*>(packed);
			for (uint32_t i = 0; i < count; i++) {
				put(i, static_cast<uint8_t>(halfs[i * 4 + 0] >> 8),
				    static_cast<uint8_t>(halfs[i * 4 + 1] >> 8),
				    static_cast<uint8_t>(halfs[i * 4 + 2] >> 8),
				    static_cast<uint8_t>(halfs[i * 4 + 3] >> 8));
			}
			return;
		}
		case vk::Format::eR32G32B32A32Sfloat: {
			const auto* floats = reinterpret_cast<const float*>(packed);
			for (uint32_t i = 0; i < count; i++) {
				put(i, Saturate8(floats[i * 4 + 0]), Saturate8(floats[i * 4 + 1]),
				    Saturate8(floats[i * 4 + 2]), Saturate8(floats[i * 4 + 3]));
			}
			return;
		}
		default:
			for (uint32_t i = 0; i < count; i++) {
				const uint32_t p = words[i];
				put(i, static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 8),
				    static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 24));
			}
			return;
	}
}

[[nodiscard]] uint32_t DumpBytesPerPixel(vk::Format format) {
	switch (format) {
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16G16B16A16Snorm: return 8;
		case vk::Format::eR32G32B32A32Sfloat: return 16;
		default: return 4;
	}
}

[[nodiscard]] bool DumpFormatSupported(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eB10G11R11UfloatPack32:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR32G32B32A32Sfloat: return true;
		default: return false;
	}
}

[[nodiscard]] bool ShouldDumpGpuImage(int frame_num) {
	if (g_gpu_dump_requested.exchange(0, std::memory_order_acq_rel) != 0) {
		return true;
	}
	switch (frame_num) {
		case 1:
		case 30:
		case 90:
		case 180:
		case 360:
		case 720:
		case 1500: return true;
		default: break;
	}
	std::error_code ec;
	if (std::filesystem::exists("D:/PS5/dumps/DUMP_NOW", ec)) {
		std::filesystem::remove("D:/PS5/dumps/DUMP_NOW", ec);
		return true;
	}
	return false;
}

void DumpGpuImage(CommandBuffer& command, RenderContext& renderer, Image& image, const char* tag,
                  uint64_t address, bool dump) {
	if (!dump || command.IsInvalid() || image.backing.image == nullptr || !image.IsGpuModified()) {
		return;
	}
	const auto width  = image.backing.extent.width;
	const auto height = image.backing.extent.height;
	if (!DumpFormatSupported(image.backing.format)) {
		return;
	}
	if (width == 0 || height == 0 || image.backing.extent.depth != 1) {
		return;
	}
	const auto frame_num = renderer.GetGpu().GetFrameNum();

	const uint64_t byte_size =
	    static_cast<uint64_t>(width) * height * DumpBytesPerPixel(image.backing.format);
	auto&          scheduler = renderer.GetCommandScheduler();
	auto&          download  = renderer.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset]    = download.Map(byte_size, 256);
	if (mapped == nullptr) {
		LOGF("VideoOut dump: failed to map download buffer for %s\n", tag);
		return;
	}

	command.EndRendering();
	auto vk_command = command.Handle();
	image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	              vk_command);
	vk::BufferImageCopy copy {};
	copy.bufferOffset                    = offset;
	copy.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	copy.imageSubresource.mipLevel       = 0;
	copy.imageSubresource.baseArrayLayer = 0;
	copy.imageSubresource.layerCount     = 1;
	copy.imageExtent                     = {width, height, 1};
	vk::BufferMemoryBarrier2 to_copy {};
	to_copy.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	to_copy.srcAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	to_copy.dstStageMask        = vk::PipelineStageFlagBits2::eCopy;
	to_copy.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_copy.buffer              = download.Handle();
	to_copy.offset              = offset;
	to_copy.size                = byte_size;
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &to_copy;
	vk_command.pipelineBarrier2(dependency);
	vk_command.copyImageToBuffer(image.backing.image, vk::ImageLayout::eTransferSrcOptimal,
	                             download.Handle(), 1, &copy);
	to_copy.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	to_copy.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	to_copy.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
	to_copy.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	vk_command.pipelineBarrier2(dependency);
	download.Commit();

	const auto      format   = image.backing.format;
	const std::string tag_copy = tag;
	scheduler.DeferPriorityOperation(
	    [&download, mapped, offset, byte_size, width, height, format, frame_num, tag_copy,
	     address] {
		    download.Invalidate(offset, byte_size);
		    std::vector<uint8_t> packed(mapped, mapped + byte_size);
		    std::vector<uint8_t> bgra;
		    uint64_t             nonzero     = 0;
		    uint32_t             max_channel = 0;
		    ConvertPackedToBgra(format, width, height, packed.data(), bgra, nonzero, max_channel);
		    std::error_code ec;
		    std::filesystem::create_directories("D:/PS5/dumps", ec);
		    const auto path = std::filesystem::path("D:/PS5/dumps") /
		                      (tag_copy + "-f" + std::to_string(frame_num) + ".bmp");
		    WriteBmpBgra(path, width, height, bgra);
		    LOGF("VideoOut dump: %s frame=%d fmt=%d addr=0x%016" PRIx64
		         " extent=%ux%u nonzero=%" PRIu64 "/%u max8=%u file=%s\n",
		         tag_copy.c_str(), frame_num, static_cast<int>(format), address, width, height,
		         nonzero, width * height, max_channel, path.string().c_str());
	    });
}

void DumpUfcSurfaces(CommandBuffer& command, RenderContext& renderer, TextureCache& cache,
                     Image& scanout, Image& presented, uint64_t scanout_address) {
	const bool dump = ShouldDumpGpuImage(renderer.GetGpu().GetFrameNum());
	if (!dump) {
		return;
	}
	DumpGpuImage(command, renderer, scanout, "display", scanout_address, true);
	DumpGpuImage(command, renderer, presented, "present", presented.info.data.address, true);
	static constexpr uint64_t kSurfaces[] = {
	    0x0000001162c00000ull, 0x0000001163470000ull, 0x0000001167150000ull,
	    0x0000001169860000ull, 0x0000001168270000ull, 0x0000001168260000ull,
	    0x0000001164240000ull, 0x0000001164e50000ull, 0x00000011592b0000ull,
	};
	for (const auto address: kSurfaces) {
		auto id = cache.FindImageFromRange(address, 0x0000000002000000ull, false);
		if (!id) {
			continue;
		}
		char tag[32];
		std::snprintf(tag, sizeof(tag), "rt%08x", static_cast<uint32_t>(address));
		DumpGpuImage(command, renderer, cache.GetImage(id), tag, address, true);
	}
}

} // namespace

struct Presenter::Frame {
	VulkanImage image;
	uint64_t    present_tick = 0;
	bool        busy         = false;
	bool        reusing_last = false;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			EXIT("prepared-frame pool was initialized twice\n");
		}
		m_format = format;
		m_frames.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires a presentation format\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	Presenter::Frame* Acquire() {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		while (m_free.empty()) {
			m_available.Wait(&m_mutex);
		}
		auto* frame = m_free.front();
		m_free.pop_front();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		if (m_last_frame == frame) {
			m_last_frame = nullptr;
		}
		frame->busy         = true;
		frame->reusing_last = false;
		m_mutex.Unlock();

		WaitForFrame(*frame);
		return frame;
	}

	Presenter::Frame* AcquireLast() {
		m_mutex.Lock();
		auto* frame = m_last_frame;
		if (frame == nullptr) {
			m_mutex.Unlock();
			return nullptr;
		}
		auto free = std::find(m_free.begin(), m_free.end(), frame);
		if (free == m_free.end() || frame->busy) {
			m_mutex.Unlock();
			EXIT("last submitted frame is not available for reuse\n");
		}
		m_free.erase(free);
		m_last_frame        = nullptr;
		frame->busy         = true;
		frame->reusing_last = true;
		m_mutex.Unlock();

		WaitForFrame(*frame);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame, bool reuse) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy || frame->reusing_last != reuse) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame, bool make_last = false) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy         = false;
		frame->reusing_last = false;
		if (make_last) {
			m_last_frame = frame;
		}
		m_free.push_back(frame);
		m_available.Signal();
	}

private:
	void WaitForFrame(Presenter::Frame& frame) { m_scheduler.Wait(frame.present_tick); }

	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	Common::CondVar                                m_available;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::deque<Presenter::Frame*>                  m_free;
	Presenter::Frame*                              m_last_frame = nullptr;
	vk::Format                                     m_format     = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
	    vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}
	if (dst.image != nullptr) {
		graphics.DeleteImage(dst);
		dst.memory = {};
	}

	dst.extent     = {extent.width, extent.height, 1};
	dst.format     = format;
	dst.layers     = 1;
	dst.mip_levels = 1;
	dst.state      = {};
	dst.subresource_states.clear();
	dst.memory.property = vk::MemoryPropertyFlagBits::eDeviceLocal;

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {dst.extent.width, dst.extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = dst.format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     dst.extent.width, dst.extent.height, static_cast<int>(dst.format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const auto width  = std::min(source.backing.extent.width, image.extent.width);
	const auto height = std::min(source.backing.extent.height, image.extent.height);
	const auto layers = std::min(source.backing.layers, image.layers);
	EXIT_IF(layers == 0);
	// copyImage requires identical formats. VideoOut 10:10:10:2 and shader storage images can
	// disagree on A2R vs A2B packing while still sharing one texture-cache backing, so blit
	// when the present image was allocated with a different host format.
	if (source.backing.format == image.format) {
		vk::ImageCopy copy {};
		copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, layers};
		copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, layers};
		copy.extent         = {width, height, 1};
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
		                  vk::ImageLayout::eTransferDstOptimal, copy);
	} else {
		vk::ImageBlit blit {};
		blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, layers};
		blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, layers};
		blit.srcOffsets[1]  = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
		blit.dstOffsets[1]  = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
		command.blitImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
		                  vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
	}
	Transit(command, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
	Transit(command, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	void                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] Status AcquireNextImage();
	[[nodiscard]] bool   PrepareSystemOverlay();
	void                 RecordPresentCommands(CommandBuffer& command, VulkanImage& source,
	                                           bool draw_system_overlay);
	uint64_t             Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }

private:
	void Destroy();

	WindowContext&              m_window;
	vk::SwapchainKHR            m_handle = nullptr;
	vk::Format                  m_format = vk::Format::eUndefined;
	vk::Extent2D                m_extent {};
	std::vector<vk::Image>      m_images;
	std::vector<vk::ImageView>  m_image_views;
	std::vector<vk::Semaphore>  m_image_acquired;
	std::vector<vk::Semaphore>  m_render_complete;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	uint32_t                    m_image_index = static_cast<uint32_t>(-1);
	uint32_t                    m_frame_index = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx), frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
		swapchain.Create();
		frames.Initialize(swapchain.ImageCount(), swapchain.Format());
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		frames.SetFormat(swapchain.Format());
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	std::atomic<uint64_t> presented_overlay_revision {0};
};

void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);

	Common::LockGuard lock(m_window.mutex);
	EXIT_IF(graphics.screen_width == 0);
	EXIT_IF(graphics.screen_height == 0);
	const auto&       surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(graphics.screen_width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(graphics.screen_height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	vk::SurfaceFormatKHR format {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	if (surface.formats.size() != 1 || surface.formats.front().format != vk::Format::eUndefined) {
		const auto it = std::find_if(surface.formats.begin(), surface.formats.end(),
		                             [](const vk::SurfaceFormatKHR& candidate) {
			                             return candidate.format == vk::Format::eB8G8R8A8Unorm ||
			                                    candidate.format == vk::Format::eR8G8B8A8Unorm;
		                             });
		if (it == surface.formats.end()) {
			EXIT("no supported UNORM swapchain format\n");
		}
		format = *it;
	}
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped          = VK_TRUE;
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
}

Swapchain::~Swapchain() {
	Destroy();
}

void Swapchain::Destroy() {
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle      = nullptr;
	m_format      = vk::Format::eUndefined;
	m_extent      = {};
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_render_complete.clear();
}

void Swapchain::Recreate(bool surface_lost) {
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		m_window.RunOnMainThread([this] { m_window.RecreateSurface(); });
#else
		m_window.RecreateSurface();
#endif
	}
	m_window.RefreshSurfaceCapabilities();
	Create();
}

Swapchain::Status Swapchain::AcquireNextImage() {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size());
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, VulkanImage& source,
                                      bool draw_system_overlay) {
	if (source.state.layout != vk::ImageLayout::eTransferSrcOptimal) {
		EXIT("invalid prepared presentation image, vk_image=%p layout=%d\n",
		     static_cast<void*>(source.image), static_cast<int>(source.state.layout));
	}
	EXIT_IF(m_image_index >= m_images.size());
	auto vk_command = command.Handle();

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType                           = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout                       = vk::ImageLayout::eUndefined;
	to_transfer.newLayout                       = vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	vk::ImageBlit region {};
	region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.srcSubresource.mipLevel       = 0;
	region.srcSubresource.baseArrayLayer = 0;
	region.srcSubresource.layerCount     = 1;
	region.srcOffsets[1].x               = static_cast<int>(source.extent.width);
	region.srcOffsets[1].y               = static_cast<int>(source.extent.height);
	region.srcOffsets[1].z               = 1;
	region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.dstSubresource.mipLevel       = 0;
	region.dstSubresource.baseArrayLayer = 0;
	region.dstSubresource.layerCount     = 1;
	region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
	region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
	region.dstOffsets[1].z               = 1;
	vk_command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal,
	                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1, &region,
	                     vk::Filter::eLinear);

	vk::ImageMemoryBarrier to_present {};
	to_present.sType         = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask = draw_system_overlay ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                     vk::AccessFlagBits::eColorAttachmentWrite
	                                               : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout     = draw_system_overlay ? vk::ImageLayout::eColorAttachmentOptimal
	                                               : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_present.image                           = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eTransfer,
	    draw_system_overlay ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                        : vk::PipelineStageFlagBits::eAllCommands,
	    vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1, &to_present);
	if (draw_system_overlay) {
		m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
}

Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto*             frame = m_impl->frames.Acquire();
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	auto&             scanout = m_impl->ResolveSurface(info);
	if (scanout.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&scanout));
	}

	auto&  cache  = m_impl->renderer.GetTextureCache();
	Image* source = &scanout;
	static std::atomic<uint32_t> ufc_present_logs = 0;
	const auto consider = [&](ImageId id, const char* tag, bool allow_scanout_addr) {
		if (!id || source != &scanout) {
			return;
		}
		auto& candidate = cache.GetImage(id);
		if (&candidate == &scanout) {
			return;
		}
		if (!allow_scanout_addr && (candidate.info.data.address == info.data.address ||
		                            candidate.info.data.address == 0x000000111a800000ull ||
		                            candidate.info.data.address == 0x000000111b800000ull)) {
			return;
		}
		if (!candidate.IsGpuModified() || candidate.backing.image == nullptr ||
		    candidate.backing.extent.width < 1280u || candidate.backing.extent.height < 720u) {
			return;
		}
		source = &candidate;
		if (ufc_present_logs.fetch_add(1, std::memory_order_relaxed) < 12) {
			LOGF("UFC 5 present %s: fmt=%d gpu=%d rt=%d storage=%d extent=%ux%u "
			     "addr=0x%016" PRIx64 " scanout=0x%016" PRIx64 "\n",
			     tag, static_cast<int>(candidate.backing.format),
			     candidate.IsGpuModified() ? 1 : 0, candidate.usage.render_target ? 1 : 0,
			     candidate.usage.storage ? 1 : 0, candidate.backing.extent.width,
			     candidate.backing.extent.height, candidate.info.data.address, info.data.address);
		}
	};
	// The game composites its final frame into a B10G11R11 storage image aliased at the
	// flip address, but registers the VideoOut buffer with a different (color) format,
	// so ResolveSurface() hands back a distinct image the compositor never writes.
	// Present the GPU-written alias at the flip address first - that is the real frame.
	consider(cache.FindImageFromRange(info.data.address, 0x0000000000870000ull, false),
	         "flip alias", true);
	// Fall back to the last large GPU-written color / the title compositor only when the
	// flip alias had nothing (early boot, menu paths that render straight to 0x1162c00000).
	consider(cache.FindLastPresentableColor(), "last color", false);
	consider(cache.FindImageFromRange(0x0000001162c00000ull, 0x0000000000870000ull, false),
	         "compositor color", false);
	auto& image = *source;
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}

	auto frame_format = image.backing.format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	if (info.pixel_format != image.backing.format) {
		static std::atomic<uint32_t> mismatch_logs = 0;
		if (mismatch_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("VideoOut present format mismatch: attribute=%d backing=%d extent=%ux%u "
			     "addr=0x%016" PRIx64 "\n",
			     static_cast<int>(info.pixel_format), static_cast<int>(image.backing.format),
			     image.backing.extent.width, image.backing.extent.height, info.data.address);
		}
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	{
		static std::atomic<uint32_t> present_logs = 0;
		if (present_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("VideoOut present: addr=0x%016" PRIx64 " attr=%d backing=%d gpu_mod=%d cpu=%d "
			     "maybe=%d buf=%d rt=%d storage=%d vo=%d extent=%ux%u\n",
			     info.data.address, static_cast<int>(info.pixel_format),
			     static_cast<int>(image.backing.format), image.IsGpuModified() ? 1 : 0,
			     image.IsDefinitelyCpuDirty() ? 1 : 0, image.IsMaybeCpuDirty() ? 1 : 0,
			     image.IsBufferModified() ? 1 : 0, image.usage.render_target ? 1 : 0,
			     image.usage.storage ? 1 : 0, image.usage.video_out ? 1 : 0,
			     image.backing.extent.width, image.backing.extent.height);
		}
	}
	if (!image.IsGpuModified()) {
		vk::ClearColorValue marker {};
		marker.float32[0] = 1.0f;
		marker.float32[2] = 1.0f;
		marker.float32[3] = 1.0f;
		frame->Clear(buffer, marker);
	} else {
		frame->CopyFrom(buffer, image);
	}
	DumpUfcSurfaces(buffer, m_impl->renderer, cache, scanout, image, info.data.address);
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire();
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

Presenter::Frame* Presenter::PrepareLastFrame() {
	return m_impl->frames.AcquireLast();
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame, bool reuse) {
	KYTY_PROFILER_FUNCTION();
	m_impl->frames.ValidateForPresent(&frame, reuse);

	const auto overlay_visual = GetSystemOverlayVisualState();
	auto&      swapchain  = m_impl->swapchain;
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		{
			FrameWorkScope present_work(FrameWorkKind::Present);
			auto status = swapchain.AcquireNextImage();
			if (status != Swapchain::Status::Success) {
				m_impl->RecoverSwapchain(status);
				continue;
			}
			{
				Common::LockGuard render_lock(m_impl->renderer.GetMutex());
				auto&             command          = m_impl->present_scheduler.BeginCommand();
				const bool        draw_system_overlay =
				    overlay_visual.active && swapchain.PrepareSystemOverlay();
				swapchain.RecordPresentCommands(command, frame.image, draw_system_overlay);
				frame.present_tick = swapchain.Submit(m_impl->present_scheduler);
			}
			status = swapchain.Present();
			if (status != Swapchain::Status::Success) {
				m_impl->RecoverSwapchain(status);
				continue;
			}
		}

		m_impl->presented_overlay_revision.store(overlay_visual.revision,
		                                         std::memory_order_release);
		m_impl->window.UpdateTitle();
		m_impl->frames.Release(&frame, true);
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
	m_impl->frames.Release(&frame, reuse);
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
