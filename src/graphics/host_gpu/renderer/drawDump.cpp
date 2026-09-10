#include "graphics/host_gpu/renderer/drawDump.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) EXIT_IF(!(x))
#include "stb_image_write.h"

namespace Libs::Graphics {

namespace {

// Layout a supported vk::Format's bytes must be decoded through to become RGBA8 for PNG
// output. Kept to the formats actually used for color render targets in this renderer;
// anything else is a clearly-logged skip rather than a silent guess, per the project's "never
// silently degrade" logging rule.
//
// RGBA16F/RG11B10F were the reason this investigation could not see its own culprit draw: both
// are real, live color-target formats in this renderer (vulkanCommon.cpp maps k16_16_16_16Float
// / k11_11_10Float straight to them) but neither was decoded here -- every HDR intermediate
// silently produced zero PNGs, so the investigation was reading the final 8-bit swapchain
// composite and inferring backwards instead of looking at the actual corrupted pass.
enum class ChannelLayout { RGBA8, BGRA8, RGBA16F, RG11B10F };

struct FormatInfo {
	vk::Format    format;
	uint32_t      bytes_per_pixel;
	ChannelLayout layout;
};

constexpr FormatInfo SUPPORTED_FORMATS[] = {
    {vk::Format::eR8G8B8A8Unorm, 4, ChannelLayout::RGBA8},
    {vk::Format::eR8G8B8A8Srgb, 4, ChannelLayout::RGBA8},
    {vk::Format::eB8G8R8A8Unorm, 4, ChannelLayout::BGRA8},
    {vk::Format::eB8G8R8A8Srgb, 4, ChannelLayout::BGRA8},
    {vk::Format::eR16G16B16A16Sfloat, 8, ChannelLayout::RGBA16F},
    {vk::Format::eB10G11R11UfloatPack32, 4, ChannelLayout::RG11B10F},
};

bool FindFormatInfo(vk::Format format, FormatInfo* out) {
	for (const auto& entry: SUPPORTED_FORMATS) {
		if (entry.format == format) {
			*out = entry;
			return true;
		}
	}
	return false;
}

bool FormatIsHdr(ChannelLayout layout) {
	return layout == ChannelLayout::RGBA16F || layout == ChannelLayout::RG11B10F;
}

// IEEE-754 binary16 -> float32. Standard bit-level decode (sign/exp/mantissa widen, subnormals
// renormalized, inf/NaN preserved) -- nothing format-specific here.
float DecodeHalf(uint16_t h) {
	const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16u;
	uint32_t       exp  = (h >> 10u) & 0x1Fu;
	uint32_t       mant = h & 0x3FFu;
	uint32_t       bits;
	if (exp == 0) {
		if (mant == 0) {
			bits = sign;
		} else {
			exp = 127u - 15u + 1u;
			while ((mant & 0x400u) == 0u) {
				mant <<= 1u;
				exp--;
			}
			mant &= 0x3FFu;
			bits = sign | (exp << 23u) | (mant << 13u);
		}
	} else if (exp == 0x1Fu) {
		bits = sign | 0x7F800000u | (mant << 13u);
	} else {
		bits = sign | ((exp - 15u + 127u) << 23u) | (mant << 13u);
	}
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

// Unsigned mini-float decode shared by the R/G (6 mantissa bits) and B (5 mantissa bits) lanes
// of VK_FORMAT_B10G11R11_UFLOAT_PACK32 -- 5-bit exponent, bias 15, no sign bit, per the Vulkan/
// GL_EXT_packed_float bit layout.
float DecodeUnsignedPackedFloat(uint32_t bits, uint32_t mantissa_bits) {
	const uint32_t mantissa_mask = (1u << mantissa_bits) - 1u;
	const uint32_t mantissa      = bits & mantissa_mask;
	const uint32_t exponent      = bits >> mantissa_bits;
	if (exponent == 0u) {
		return mantissa == 0u
		           ? 0.0f
		           : std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
	}
	if (exponent == 0x1Fu) {
		return mantissa == 0u ? std::numeric_limits<float>::infinity()
		                      : std::numeric_limits<float>::quiet_NaN();
	}
	const float normalized =
	    1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits);
	return std::ldexp(normalized, static_cast<int>(exponent) - 15);
}

void DecodeR11G11B10F(uint32_t packed, float* r, float* g, float* b) {
	*r = DecodeUnsignedPackedFloat(packed & 0x7FFu, 6u);
	*g = DecodeUnsignedPackedFloat((packed >> 11u) & 0x7FFu, 6u);
	*b = DecodeUnsignedPackedFloat((packed >> 22u) & 0x3FFu, 5u);
}

// Fixed, documented tonemap policy for HDR draw-dump PNGs: standard Reinhard (c / (1+c)) per
// channel, then the real sRGB OETF (not a bare gamma approximation). This is a visualization
// choice, not a claim about guest color management -- the filename's `_hdrR` suffix (see
// DumpColorAttachments) exists precisely so a PNG produced by this path can never be misread as
// linear truth or as the guest's own tonemap.
float ReinhardTonemap(float linear) { return linear / (1.0f + std::max(linear, 0.0f)); }

uint8_t EncodeSrgbByte(float linear) {
	linear             = std::clamp(linear, 0.0f, 1.0f);
	const float encoded = linear <= 0.0031308f ? linear * 12.92f
	                                           : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
	return static_cast<uint8_t>(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
}

// Decodes `bytes` (in `layout`, `bytes_per_pixel`-per-pixel) into a tightly-packed RGBA8 buffer.
// For the two HDR layouts, also fills `hdr_min`/`hdr_max` with the linear, pre-tonemap min/max
// seen per RGB channel (alpha excluded) -- per the owner's logging rule, so a PNG that
// tone-mapped to near-black is distinguishable from a target that was genuinely empty, instead
// of both looking identical in the output image.
std::vector<uint8_t> DecodeToRgba8(std::span<const uint8_t> bytes, uint32_t width, uint32_t height,
                                   ChannelLayout layout, float hdr_min[3], float hdr_max[3]) {
	std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
	if (FormatIsHdr(layout)) {
		hdr_min[0] = hdr_min[1] = hdr_min[2] = std::numeric_limits<float>::infinity();
		hdr_max[0] = hdr_max[1] = hdr_max[2] = -std::numeric_limits<float>::infinity();
	}
	const size_t pixel_count = static_cast<size_t>(width) * height;
	for (size_t p = 0; p < pixel_count; p++) {
		float r = 0, g = 0, b = 0, a = 1.0f;
		switch (layout) {
			case ChannelLayout::RGBA8: {
				const auto* px  = &bytes[p * 4];
				out[p * 4 + 0]  = px[0];
				out[p * 4 + 1]  = px[1];
				out[p * 4 + 2]  = px[2];
				out[p * 4 + 3]  = px[3];
				continue;
			}
			case ChannelLayout::BGRA8: {
				const auto* px  = &bytes[p * 4];
				out[p * 4 + 0]  = px[2];
				out[p * 4 + 1]  = px[1];
				out[p * 4 + 2]  = px[0];
				out[p * 4 + 3]  = px[3];
				continue;
			}
			case ChannelLayout::RGBA16F: {
				uint16_t half[4];
				std::memcpy(half, &bytes[p * 8], sizeof(half));
				r = DecodeHalf(half[0]);
				g = DecodeHalf(half[1]);
				b = DecodeHalf(half[2]);
				a = std::clamp(DecodeHalf(half[3]), 0.0f, 1.0f);
				break;
			}
			case ChannelLayout::RG11B10F: {
				uint32_t packed;
				std::memcpy(&packed, &bytes[p * 4], sizeof(packed));
				DecodeR11G11B10F(packed, &r, &g, &b);
				a = 1.0f;
				break;
			}
		}
		float channels[3] = {r, g, b};
		for (int c = 0; c < 3; c++) {
			hdr_min[c] = std::min(hdr_min[c], channels[c]);
			hdr_max[c] = std::max(hdr_max[c], channels[c]);
		}
		out[p * 4 + 0] = EncodeSrgbByte(ReinhardTonemap(r));
		out[p * 4 + 1] = EncodeSrgbByte(ReinhardTonemap(g));
		out[p * 4 + 2] = EncodeSrgbByte(ReinhardTonemap(b));
		out[p * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
	}
	return out;
}

void EncodeAndWrite(std::vector<uint8_t> bytes, uint32_t width, uint32_t height,
                    ChannelLayout layout, std::filesystem::path path) {
	float      hdr_min[3] {};
	float      hdr_max[3] {};
	auto       rgba8     = DecodeToRgba8(bytes, width, height, layout, hdr_min, hdr_max);
	const auto path_text = Common::PathToString(path);
	if (FormatIsHdr(layout)) {
		LOGF("draw-dump: %s tonemap=ReinhardSrgb rgb_min=(%.4f,%.4f,%.4f)"
		     " rgb_max=(%.4f,%.4f,%.4f)\n",
		     path_text.c_str(), hdr_min[0], hdr_min[1], hdr_min[2], hdr_max[0], hdr_max[1],
		     hdr_max[2]);
	}
	Common::File::CreateDirectories(path.parent_path());
	if (!stbi_write_png(path_text.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
	                    rgba8.data(), static_cast<int>(width) * 4)) {
		LOGF_COLOR(Log::Color::BrightRed, "draw-dump: failed to write %s\n", path_text.c_str());
	}
}

// Companion index for every dump, alongside the LOGF line below: same information, but a file
// a script can read straight away instead of scraping the console/log for a matching prefix.
// Best-effort (a failure here never affects the dump itself, which is why every early-out is a
// silent `return` rather than an EXIT): the LOGF line remains the source of truth if this ever
// fails to write, e.g. a full disk.
//
// Schema version for this CSV's column layout. Bump this whenever the column list changes; the
// writer below refuses to append to a file whose on-disk header doesn't match (starts a fresh,
// distinctly-named file instead) so a column-count change can never silently corrupt an
// existing index.csv from a prior run -- previously there was no version at all and an
// append-only header made that exact corruption possible.
constexpr int DRAW_DUMP_INDEX_SCHEMA = 2;

std::string DrawDumpIndexHeader() {
	// own_addr: the guest address THIS render target's own image is backed by -- added in
	// schema 2. Without it, a composite draw's tex0/tex1 (what it *reads*) can never be joined
	// back to the earlier dump that *wrote* that same address, which is exactly the join this
	// investigation needed and didn't have (see workflow/astro_playroom_issues.md, session 6).
	std::string header = fmt::format("schema,frame,index,target,prim,vs_hash,ps_hash,own_addr");
	for (uint32_t t = 0; t < RenderAttachment::TEX_DEBUG_MAX; t++) {
		header += fmt::format(",tex{}_addr,tex{}_fmt,tex{}_tile", t, t, t);
	}
	header += '\n';
	return header;
}

// Bug caught this session: own_addr was added to the header string and to this function's
// signature, but never actually appended to the row itself below -- every column after ps_hash
// silently shifted left by one (tex0_fmt's value landing in the tex0_addr column, etc.) in any
// row written before this fix. Never actually exercised end-to-end until now: the call site
// (drawDump.cpp's DumpColorAttachments) didn't compile with the extra argument, so no row with
// this bug ever reached disk before this session added the missing argument at the call site.
void AppendDrawDumpIndexRow(uint32_t frame, uint32_t index, uint32_t target, uint64_t own_addr,
                            const RenderAttachment& attachment) {
	static std::mutex mutex;
	std::lock_guard   lock(mutex);

	auto       path       = Config::GetDrawDumpFolder() / "index.csv";
	const auto new_header = DrawDumpIndexHeader();
	// File::Printf rewrites every '\n' to "\r\n" before writing (see file.cpp), so the header
	// actually on disk is one byte longer than `new_header` -- compare against that, not the
	// pre-write string, or every schema check after the first ever call would spuriously "fail".
	const auto on_disk_header = Common::ReplaceStr(new_header, "\n", "\r\n");

	if (Common::File::IsFileExisting(path)) {
		Common::File existing;
		bool         schema_matches = false;
		if (existing.Open(path, Common::File::Mode::Read)) {
			std::string first_line(on_disk_header.size(), '\0');
			uint32_t    bytes_read = 0;
			existing.Read(first_line.data(), static_cast<uint32_t>(first_line.size()), &bytes_read);
			first_line.resize(bytes_read);
			schema_matches = first_line == on_disk_header;
		}
		if (!schema_matches) {
			// A prior run's index.csv has a different column layout: never append columns that
			// don't line up, and never silently overwrite the old file either -- start a new,
			// clearly-named one instead so both remain readable.
			path = Config::GetDrawDumpFolder() /
			      fmt::format("index_schema{}.csv", DRAW_DUMP_INDEX_SCHEMA);
		}
	}

	const bool is_new = !Common::File::IsFileExisting(path);
	Common::File file;
	if (is_new) {
		Common::File::CreateDirectories(path.parent_path());
		if (!file.Create(path)) {
			return;
		}
		file.Printf("%s", new_header.c_str());
	} else {
		if (!file.Open(path, Common::File::Mode::Write)) {
			return;
		}
		file.Seek(file.Size());
	}

	std::string row =
	    fmt::format("{},{},{},{},{},0x{:016x},0x{:016x},0x{:016x}", DRAW_DUMP_INDEX_SCHEMA, frame,
	               index, target, attachment.prim_type, attachment.vs_shader_hash,
	               attachment.ps_shader_hash, own_addr);
	for (uint32_t t = 0; t < RenderAttachment::TEX_DEBUG_MAX; t++) {
		row += fmt::format(",0x{:016x},{},{}", attachment.tex_address[t], attachment.tex_format[t],
		                   attachment.tex_tile_mode[t]);
	}
	row += '\n';
	file.Printf("%s", row.c_str());
}

} // namespace

// --draw-dump owns this buffer outright instead of sharing BufferCache's 32 MiB Download ring
// (BufferCache::GetUtilityBuffer(MemoryUsage::Download)): TextureCache::TryDownloadImage and
// BufferCache::DownloadBufferMemory both EXIT on a failed reservation against that ring, so a
// sustained dump run wrapping it every ~4 attachments forced a full GPU stall
// (StreamBuffer::WaitPendingOperations) on every wrap, contending with real rendering and
// starving those other consumers. 64 MiB covers two
// full 3840x2160 RGBA8 attachments. Lazily constructed: there is exactly one RenderContext for
// the process's lifetime, so a function-local instance never outlives the GraphicContext it was
// built from.
StreamBuffer& DrawDumpDownloadBuffer(RenderContext& context) {
	static std::unique_ptr<StreamBuffer> buffer;
	if (!buffer) {
		buffer = std::make_unique<StreamBuffer>(context.GetGraphics(), context.GetCommandScheduler(),
		                                        MemoryUsage::Download, 64u * 1024u * 1024u);
	}
	return *buffer;
}

// Separate static instance from DrawDumpDownloadBuffer's: that one is permanently bound to
// whichever RenderContext/CommandScheduler first constructs it, and the present path runs on
// Presenter's own present_scheduler, a different CommandScheduler than the main render context's.
// Sharing one StreamBuffer across two unrelated schedulers would let one submission's tick gate a
// completely unrelated one's readback.
StreamBuffer& PresentDumpDownloadBuffer(GraphicContext& graphics, CommandScheduler& scheduler) {
	static std::unique_ptr<StreamBuffer> buffer;
	if (!buffer) {
		buffer = std::make_unique<StreamBuffer>(graphics, scheduler, MemoryUsage::Download,
		                                        64u * 1024u * 1024u);
	}
	return *buffer;
}

Common::UniqueFunction<void> DumpPresentedFrame(CommandBuffer& command,
                                                  CommandScheduler& scheduler, VulkanImage& source,
                                                  int frame_num) {
	if (!Config::PresentDumpEnabled()) {
		return {};
	}
	const auto every = Config::GetPresentDumpEvery();
	if (every <= 0 || frame_num % every != 0) {
		return {};
	}

	FormatInfo format_info {};
	if (!FindFormatInfo(source.format, &format_info)) {
		LOGF_COLOR(Log::Color::Yellow, "present-dump: frame %d uses unsupported format %s, skipping\n",
		          frame_num, vk::to_string(source.format).c_str());
		return {};
	}
	const auto     layout = format_info.layout;
	const uint32_t width  = source.extent.width;
	const uint32_t height = source.extent.height;
	const uint64_t size   = uint64_t {width} * height * format_info.bytes_per_pixel;
	if (width == 0 || height == 0) {
		return {};
	}

	auto& download = PresentDumpDownloadBuffer(command.GetGraphics(), scheduler);
	auto [mapped, offset] = download.Map(size, 4);
	if (mapped == nullptr) {
		LOGF_COLOR(Log::Color::BrightRed, "present-dump: failed to reserve download space\n");
		return {};
	}
	download.Commit();

	// Mirrors Image::Download's buffer-side barrier pair (image.cpp) exactly: `source` is already
	// confirmed vk::ImageLayout::eTransferSrcOptimal by Swapchain::RecordPresentCommands before
	// this is called, so no image barrier is needed here, only the read-after-write/
	// write-after-read guard around this ring-buffer slot's reuse.
	auto                     vk_command = command.Handle();
	vk::BufferMemoryBarrier2 buffer_barrier {};
	buffer_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eCopy;
	buffer_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer              = download.Handle();
	buffer_barrier.offset              = offset;
	buffer_barrier.size                = size;
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &buffer_barrier;
	vk_command.pipelineBarrier2(dependency);

	vk::BufferImageCopy region {};
	region.bufferOffset                    = offset;
	region.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.imageSubresource.mipLevel       = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount     = 1;
	region.imageExtent                     = source.extent;
	vk_command.copyImageToBuffer(source.image, vk::ImageLayout::eTransferSrcOptimal,
	                             download.Handle(), 1, &region);

	buffer_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
	buffer_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	vk_command.pipelineBarrier2(dependency);

	const auto path = Config::GetPresentDumpFolder() /
	                  fmt::format("present_{:06d}.png", frame_num);
	LOGF("present-dump: %06d\n", frame_num);
	// Cannot use scheduler.DeferOperation() here: it asserts CommandScheduler::Active(), which
	// requires a bound guest HW::Context that the present scheduler never has (see the header
	// comment). The caller waits on the submitted tick and runs this itself instead.
	return [&download, offset, size, width, height, layout, path] {
		download.Invalidate(offset, size);
		const auto mapped_now = download.Mapped().subspan(offset, size);
		EncodeAndWrite(std::vector<uint8_t>(mapped_now.begin(), mapped_now.end()), width, height,
		              layout, path);
	};
}

void DumpColorAttachments(RenderContext& context, const RenderState& state) {
	static std::atomic<uint32_t> s_dump_index {0};

	auto& textures  = context.GetTextureCache();
	auto& scheduler = context.GetCommandScheduler();
	const auto frame = context.GetGpu().GetFrameNum();
	// Found session 7 (2026-09-10): this function had NO frame-window gate at all, unlike
	// every text-log diagnostic in renderDraw.cpp/descriptors.cpp/context.cpp, which all
	// already respect --draw-log-frame-first/-last. A single --draw-dump run therefore paid a
	// synchronous GPU download + PNG encode for EVERY draw across the ENTIRE run, no matter how
	// narrow a frame window was requested elsewhere -- on a compute/draw-heavy scene (confirmed:
	// ASTRO's Playroom's animated intro issues thousands of real dispatches per GPU submission)
	// this alone made the game appear to "hang" under --draw-dump, which was misdiagnosed as a
	// render-pass-batching regression before this gate was traced down. Applying the SAME window
	// here, consistent with the rest of the diagnostic suite, rather than inventing new flags.
	const auto first = Config::GetDrawLogFrameFirst();
	const auto last   = Config::GetDrawLogFrameLast();
	if ((first >= 0 && frame < first) || (last >= 0 && frame > last)) {
		return;
	}
	auto& download = DrawDumpDownloadBuffer(context);

	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		if (!attachment.image_id) {
			continue;
		}
		auto&      image  = textures.GetImage(attachment.image_id);
		const auto format = image.backing.format;

		FormatInfo format_info {};
		if (!FindFormatInfo(format, &format_info)) {
			LOGF_COLOR(Log::Color::Yellow,
			           "draw-dump: color target %u uses unsupported format %s, skipping\n", i,
			           vk::to_string(format).c_str());
			continue;
		}
		const auto layout = format_info.layout;

		const uint32_t width  = state.width;
		const uint32_t height = state.height;
		const uint64_t size   = uint64_t {width} * height * format_info.bytes_per_pixel;
		if (width == 0 || height == 0) {
			continue;
		}

		auto [mapped, offset] = download.Map(size, 4);
		if (mapped == nullptr) {
			LOGF_COLOR(Log::Color::BrightRed, "draw-dump: failed to reserve download space\n");
			continue;
		}
		download.Commit();

		vk::BufferImageCopy region {};
		region.bufferOffset                    = offset;
		region.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.imageSubresource.mipLevel       = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount     = 1;
		region.imageExtent                     = vk::Extent3D {width, height, 1};
		image.Download(std::span<const vk::BufferImageCopy>(&region, 1), download.Handle(), offset,
		              size);

		const auto index = s_dump_index.fetch_add(1, std::memory_order_relaxed);
		// vs/ps shader hashes and the guest primitive type of the last draw into this attachment
		// are folded into the filename (see RenderAttachment::vs_shader_hash, renderTarget.h) so
		// a visual defect can be traced straight to its shader via --shader-log-folder's
		// identically-formatted {:016x} hash, without cross-referencing timestamps or log lines.
		// _hdrR: tone-mapped through EncodeAndWrite's Reinhard+sRGB policy rather than a direct
		// 8-bit copy -- stamped into the name so this PNG's pixels can never be misread as the
		// guest's own linear/tonemapped output.
		const auto path =
		    Config::GetDrawDumpFolder() /
		    fmt::format("{:04d}_{:06d}_target{}_prim{}_vs{:016x}_ps{:016x}{}.png", frame, index, i,
		               attachment.prim_type, attachment.vs_shader_hash, attachment.ps_shader_hash,
		               FormatIsHdr(layout) ? "_hdrR" : "");
		// Companion line for the bound textures (guest address/format/tile mode) -- kept as a
		// log line rather than more filename suffix, since the filename is already at the limit of
		// what's readable in a directory listing. Was tex0/tex1 only (RenderAttachment's old
		// 2-entry cap); now covers every slot RenderAttachment::TEX_DEBUG_MAX carries.
		std::string tex_summary;
		for (uint32_t t = 0; t < RenderAttachment::TEX_DEBUG_MAX; t++) {
			tex_summary += fmt::format("tex{}=[addr=0x{:016x} fmt={} tile={}] ", t,
			                          attachment.tex_address[t], attachment.tex_format[t],
			                          attachment.tex_tile_mode[t]);
		}
		LOGF("draw-dump: %04d_%06d_target%u %s\n", frame, index, i, tex_summary.c_str());
		AppendDrawDumpIndexRow(frame, index, i, image.info.data.address, attachment);

		// Deferred: PopPendingOperations() only runs this once the GPU has actually
		// finished the copy recorded above (MasterSemaphore tick tracking), so the
		// download buffer's contents are guaranteed valid by the time it fires.
		scheduler.DeferOperation([&download, offset, size, width, height, layout, path] {
			download.Invalidate(offset, size);
			const auto mapped_now = download.Mapped().subspan(offset, size);
			EncodeAndWrite(std::vector<uint8_t>(mapped_now.begin(), mapped_now.end()), width,
			              height, layout, path);
		});
	}
}

} // namespace Libs::Graphics
