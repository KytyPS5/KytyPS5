#include "graphics/host_gpu/renderer/frameDump.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#include "stb_image_write.h"

namespace Libs::Graphics::FrameDump {

std::atomic<bool> g_active {false};

namespace {

constexpr uint32_t   kMaxImages       = 400;
constexpr uint32_t   kMinSize         = 8;
constexpr uint64_t   kMaxPendingBytes = 3ull << 30u;

struct Target {
	vk::Image            image = nullptr;
	vk::Format           format = vk::Format::eUndefined;
	vk::ImageAspectFlags full_aspect = vk::ImageAspectFlagBits::eColor;
	vk::ImageAspectFlags copy_aspect = vk::ImageAspectFlagBits::eColor;
	uint32_t             mip = 0, layer = 0, w = 0, h = 0, samples = 1;
	vk::ImageLayout      layout = vk::ImageLayout::eUndefined;
	bool                 depth = false;
	uint32_t             slot = 0;
	uint64_t             addr = 0;
	bool                 dirty = false;
	uint32_t             seq = 0;
};

struct Pending {
	vk::Buffer    buffer = nullptr;
	VmaAllocation alloc  = nullptr;
	void*         mapped = nullptr;
	uint64_t      size   = 0;
	Target        target;
	uint32_t      seq = 0;
	std::string   label;
	GraphicContext* graphics = nullptr;
};

std::recursive_mutex                    g_mutex;
std::unordered_map<uint64_t, Target>    g_targets;
std::vector<Pending>                    g_pending;
std::string                             g_log;
std::string                             g_notes;
uint32_t                                g_seq           = 0;
uint32_t                                g_images        = 0;
uint64_t                                g_pending_bytes = 0;
std::string                             g_dir;
uint32_t                                g_frame = 0;
bool                                    g_capturing = false;
std::atomic<bool>                       g_request {false};

const char* TypeName(Prospero::ImageType t) {
	switch (t) {
		case Prospero::ImageType::kColor2D: return "2D";
		case Prospero::ImageType::kColor2DArray: return "2DArray";
		case Prospero::ImageType::kCube: return "Cube";
		case Prospero::ImageType::kColor3D: return "3D";
		case Prospero::ImageType::kColor1D: return "1D";
		case Prospero::ImageType::kColor2DMsaa: return "2DMsaa";
		default: return "other";
	}
}

bool HasStencil(vk::Format f) {
	return f == vk::Format::eD16UnormS8Uint || f == vk::Format::eD24UnormS8Uint ||
	       f == vk::Format::eD32SfloatS8Uint;
}

bool IsDepthFormat(vk::Format f) {
	return f == vk::Format::eD16Unorm || f == vk::Format::eD32Sfloat ||
	       f == vk::Format::eX8D24UnormPack32 || HasStencil(f);
}

uint32_t BytesPerPixel(vk::Format f) {
	switch (f) {
		case vk::Format::eR8Unorm: case vk::Format::eR8Uint: case vk::Format::eR8Snorm:
		case vk::Format::eR8Sint: return 1;
		case vk::Format::eR8G8Unorm: case vk::Format::eR8G8Uint: case vk::Format::eR8G8Snorm:
		case vk::Format::eR16Unorm: case vk::Format::eR16Sfloat: case vk::Format::eR16Uint:
		case vk::Format::eR16Snorm: case vk::Format::eR16Sint:
		case vk::Format::eR5G6B5UnormPack16: case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: return 2;
		case vk::Format::eR8G8B8A8Unorm: case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eR8G8B8A8Snorm: case vk::Format::eR8G8B8A8Uint:
		case vk::Format::eB8G8R8A8Unorm: case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32: case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA2B10G10R10UintPack32: case vk::Format::eB10G11R11UfloatPack32:
		case vk::Format::eR16G16Unorm: case vk::Format::eR16G16Sfloat: case vk::Format::eR16G16Uint:
		case vk::Format::eR32Sfloat: case vk::Format::eR32Uint: case vk::Format::eR32Sint:
		case vk::Format::eD32Sfloat: case vk::Format::eX8D24UnormPack32:
		case vk::Format::eD24UnormS8Uint: case vk::Format::eD32SfloatS8Uint: return 4;
		case vk::Format::eR16G16B16A16Sfloat: case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16G16B16A16Uint: case vk::Format::eR16G16B16A16Snorm:
		case vk::Format::eR32G32Sfloat: case vk::Format::eR32G32Uint: return 8;
		case vk::Format::eR32G32B32A32Sfloat: case vk::Format::eR32G32B32A32Uint: return 16;
		default: return 0;
	}
}

float HalfToFloat(uint16_t h) {
	const uint32_t s = (h >> 15u) & 1u, e = (h >> 10u) & 31u, m = h & 1023u;
	float v;
	if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
	else if (e == 31) v = m ? 0.0f : 65504.0f;
	else v = std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
	return s ? -v : v;
}

float SmallFloat(uint32_t v, uint32_t mant_bits) {
	const uint32_t e = v >> mant_bits, m = v & ((1u << mant_bits) - 1u);
	if (e == 0) return std::ldexp(static_cast<float>(m), -14 - static_cast<int>(mant_bits));
	if (e == 31) return m ? 0.0f : 65504.0f;
	return std::ldexp(1.0f + static_cast<float>(m) / static_cast<float>(1u << mant_bits),
	                  static_cast<int>(e) - 15);
}

enum class Kind { Ldr, Hdr, Int, Depth };

struct Decoded {
	uint32_t           w = 0, h = 0;
	int                channels = 4;
	bool               has_alpha = false;
	Kind               kind = Kind::Ldr;
	std::vector<float> px; // 4 floats per pixel
	bool               ok = false;
};

template <typename T> T Load(const uint8_t* p) {
	T v;
	std::memcpy(&v, p, sizeof(T));
	return v;
}

Decoded Decode(const Target& t, const uint8_t* src) {
	Decoded d;
	d.w = t.w;
	d.h = t.h;
	const size_t n   = static_cast<size_t>(t.w) * t.h;
	const uint32_t bpp = BytesPerPixel(t.format);
	d.px.assign(n * 4, 0.0f);
	d.ok = true;
	const auto f = t.format;
	if (t.depth) {
		d.kind = Kind::Depth;
		d.channels = 1;
		for (size_t i = 0; i < n; i++) {
			const uint8_t* p = src + i * bpp;
			float v = 0;
			if (f == vk::Format::eD16Unorm || f == vk::Format::eD16UnormS8Uint)
				v = static_cast<float>(Load<uint16_t>(p)) / 65535.0f;
			else if (f == vk::Format::eD32Sfloat || f == vk::Format::eD32SfloatS8Uint)
				v = Load<float>(p);
			else
				v = static_cast<float>(Load<uint32_t>(p) & 0xFFFFFFu) / 16777215.0f;
			d.px[i * 4] = std::isfinite(v) ? v : 0.0f;
		}
		return d;
	}
	const auto set = [&](size_t i, float r, float g, float b, float a) {
		d.px[i * 4 + 0] = r; d.px[i * 4 + 1] = g; d.px[i * 4 + 2] = b; d.px[i * 4 + 3] = a;
	};
	switch (f) {
		case vk::Format::eR8G8B8A8Unorm: case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm: case vk::Format::eB8G8R8A8Srgb: {
			const bool bgra = f == vk::Format::eB8G8R8A8Unorm || f == vk::Format::eB8G8R8A8Srgb;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 4;
				set(i, p[bgra ? 2 : 0] / 255.f, p[1] / 255.f, p[bgra ? 0 : 2] / 255.f, p[3] / 255.f);
			}
			break;
		}
		case vk::Format::eR8G8B8A8Snorm:
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const int8_t* p = reinterpret_cast<const int8_t*>(src + i * 4);
				set(i, std::max(p[0] / 127.f, 0.f), std::max(p[1] / 127.f, 0.f),
				    std::max(p[2] / 127.f, 0.f), std::max(p[3] / 127.f, 0.f));
			}
			break;
		case vk::Format::eR8G8B8A8Uint:
			d.kind = Kind::Int;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 4;
				set(i, p[0], p[1], p[2], p[3]);
			}
			break;
		case vk::Format::eA2B10G10R10UnormPack32: case vk::Format::eA2B10G10R10UintPack32:
		case vk::Format::eA2R10G10B10UnormPack32: {
			const bool argb = f == vk::Format::eA2R10G10B10UnormPack32;
			const bool is_int = f == vk::Format::eA2B10G10R10UintPack32;
			if (is_int) d.kind = Kind::Int;
			d.has_alpha = true;
			const float s = is_int ? 1.0f : 1.0f / 1023.0f;
			for (size_t i = 0; i < n; i++) {
				const uint32_t v = Load<uint32_t>(src + i * 4);
				float c0 = static_cast<float>(v & 1023u) * s, c1 = static_cast<float>((v >> 10u) & 1023u) * s,
				      c2 = static_cast<float>((v >> 20u) & 1023u) * s;
				const float a = is_int ? static_cast<float>(v >> 30u) : static_cast<float>(v >> 30u) / 3.0f;
				if (argb) std::swap(c0, c2);
				set(i, c0, c1, c2, a);
			}
			break;
		}
		case vk::Format::eB10G11R11UfloatPack32:
			d.kind = Kind::Hdr;
			d.channels = 3;
			for (size_t i = 0; i < n; i++) {
				const uint32_t v = Load<uint32_t>(src + i * 4);
				set(i, SmallFloat(v & 2047u, 6), SmallFloat((v >> 11u) & 2047u, 6),
				    SmallFloat(v >> 22u, 5), 1.0f);
			}
			break;
		case vk::Format::eR16G16B16A16Sfloat:
			d.kind = Kind::Hdr;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 8;
				set(i, HalfToFloat(Load<uint16_t>(p)), HalfToFloat(Load<uint16_t>(p + 2)),
				    HalfToFloat(Load<uint16_t>(p + 4)), HalfToFloat(Load<uint16_t>(p + 6)));
			}
			break;
		case vk::Format::eR16G16B16A16Unorm:
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 8;
				set(i, Load<uint16_t>(p) / 65535.f, Load<uint16_t>(p + 2) / 65535.f,
				    Load<uint16_t>(p + 4) / 65535.f, Load<uint16_t>(p + 6) / 65535.f);
			}
			break;
		case vk::Format::eR16G16B16A16Uint:
			d.kind = Kind::Int;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 8;
				set(i, Load<uint16_t>(p), Load<uint16_t>(p + 2), Load<uint16_t>(p + 4),
				    Load<uint16_t>(p + 6));
			}
			break;
		case vk::Format::eR32G32B32A32Sfloat:
			d.kind = Kind::Hdr;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 16;
				set(i, Load<float>(p), Load<float>(p + 4), Load<float>(p + 8), Load<float>(p + 12));
			}
			break;
		case vk::Format::eR32G32B32A32Uint:
			d.kind = Kind::Int;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 16;
				set(i, static_cast<float>(Load<uint32_t>(p)), static_cast<float>(Load<uint32_t>(p + 4)),
				    static_cast<float>(Load<uint32_t>(p + 8)), static_cast<float>(Load<uint32_t>(p + 12)));
			}
			break;
		case vk::Format::eR8Unorm: case vk::Format::eR8Uint:
			d.channels = 1;
			if (f == vk::Format::eR8Uint) d.kind = Kind::Int;
			for (size_t i = 0; i < n; i++) {
				const float v = f == vk::Format::eR8Uint ? src[i] : src[i] / 255.f;
				set(i, v, v, v, 1);
			}
			break;
		case vk::Format::eR8G8Unorm: case vk::Format::eR8G8Uint:
			d.channels = 2;
			if (f == vk::Format::eR8G8Uint) d.kind = Kind::Int;
			for (size_t i = 0; i < n; i++) {
				const float s = f == vk::Format::eR8G8Uint ? 1.f : 1.f / 255.f;
				set(i, src[i * 2] * s, src[i * 2 + 1] * s, 0, 1);
			}
			break;
		case vk::Format::eR16Unorm: case vk::Format::eR16Uint: case vk::Format::eR16Sfloat:
			d.channels = 1;
			if (f == vk::Format::eR16Uint) d.kind = Kind::Int;
			if (f == vk::Format::eR16Sfloat) d.kind = Kind::Hdr;
			for (size_t i = 0; i < n; i++) {
				const uint16_t u = Load<uint16_t>(src + i * 2);
				const float v = f == vk::Format::eR16Sfloat ? HalfToFloat(u)
				                : f == vk::Format::eR16Uint ? static_cast<float>(u) : u / 65535.f;
				set(i, v, v, v, 1);
			}
			break;
		case vk::Format::eR16G16Unorm: case vk::Format::eR16G16Uint: case vk::Format::eR16G16Sfloat:
			d.channels = 2;
			if (f == vk::Format::eR16G16Uint) d.kind = Kind::Int;
			if (f == vk::Format::eR16G16Sfloat) d.kind = Kind::Hdr;
			for (size_t i = 0; i < n; i++) {
				const uint16_t u0 = Load<uint16_t>(src + i * 4), u1 = Load<uint16_t>(src + i * 4 + 2);
				if (f == vk::Format::eR16G16Sfloat) set(i, HalfToFloat(u0), HalfToFloat(u1), 0, 1);
				else if (f == vk::Format::eR16G16Uint) set(i, u0, u1, 0, 1);
				else set(i, u0 / 65535.f, u1 / 65535.f, 0, 1);
			}
			break;
		case vk::Format::eR32Sfloat: case vk::Format::eR32Uint:
			d.channels = 1;
			d.kind = f == vk::Format::eR32Sfloat ? Kind::Hdr : Kind::Int;
			for (size_t i = 0; i < n; i++) {
				const float v = f == vk::Format::eR32Sfloat ? Load<float>(src + i * 4)
				                                              : static_cast<float>(Load<uint32_t>(src + i * 4));
				set(i, v, v, v, 1);
			}
			break;
		case vk::Format::eR32G32Sfloat: case vk::Format::eR32G32Uint:
			d.channels = 2;
			d.kind = f == vk::Format::eR32G32Sfloat ? Kind::Hdr : Kind::Int;
			for (size_t i = 0; i < n; i++) {
				if (f == vk::Format::eR32G32Sfloat)
					set(i, Load<float>(src + i * 8), Load<float>(src + i * 8 + 4), 0, 1);
				else
					set(i, static_cast<float>(Load<uint32_t>(src + i * 8)),
					    static_cast<float>(Load<uint32_t>(src + i * 8 + 4)), 0, 1);
			}
			break;
		default: d.ok = false; break;
	}
	for (auto& v : d.px) {
		if (!std::isfinite(v)) v = 0.0f;
	}
	return d;
}

uint8_t ToByte(float v) {
	return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

float Gamma(float v) {
	v = std::clamp(v, 0.0f, 1.0f);
	return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

bool WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
	return stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 3, rgb.data(),
	                      static_cast<int>(w * 3)) != 0;
}

// Maps decoded pixels through f(px) -> rgb and writes the png.
template <typename F>
bool WriteMapped(const std::string& path, const Decoded& d, F&& f) {
	std::vector<uint8_t> rgb(static_cast<size_t>(d.w) * d.h * 3);
	for (size_t i = 0; i < static_cast<size_t>(d.w) * d.h; i++) {
		float r, g, b;
		f(&d.px[i * 4], r, g, b);
		rgb[i * 3] = ToByte(r); rgb[i * 3 + 1] = ToByte(g); rgb[i * 3 + 2] = ToByte(b);
	}
	return WritePng(path, d.w, d.h, rgb);
}

std::string Hex(uint64_t v) {
	char b[32];
	std::snprintf(b, sizeof(b), "%" PRIx64, v);
	return b;
}

void ProcessOne(Pending& p) {
	if (p.mapped == nullptr) return;
	vmaInvalidateAllocation(p.graphics->allocator, p.alloc, 0, VK_WHOLE_SIZE);
	const auto& t = p.target;
	Decoded d = Decode(t, static_cast<const uint8_t*>(p.mapped));
	char base[512];
	std::snprintf(base, sizeof(base), "%s/%05u_%s_%s_%ux%u_%s", g_dir.c_str(), p.seq, p.label.c_str(),
	              Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str());
	const std::string b = base;
	char line[768];
	if (!d.ok) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u fmt=%s UNSUPPORTED (not decoded)\n",
		              p.seq, p.label.c_str(), Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str());
		g_log += line;
		return;
	}
	// Stats over the used channels.
	float mn = 3.4e38f, mx = -3.4e38f;
	const int stat_ch = d.kind == Kind::Depth ? 1 : std::min(d.channels, 3);
	const size_t n = static_cast<size_t>(d.w) * d.h;
	for (size_t i = 0; i < n; i++) {
		for (int c = 0; c < stat_ch; c++) {
			mn = std::min(mn, d.px[i * 4 + c]);
			mx = std::max(mx, d.px[i * 4 + c]);
		}
	}
	const float range = mx > mn ? mx - mn : 1.0f;
	std::string files;
	const auto note = [&](const std::string& f) {
		files += " ";
		files += std::filesystem::path(f).filename().string();
	};
	const int ch = d.channels;
	const auto chan = [ch](const float* px, float& r, float& g, float& bl, float sc, float off) {
		r = (px[0] - off) * sc;
		g = ch >= 2 ? (px[1] - off) * sc : r;
		bl = ch >= 3 ? (px[2] - off) * sc : (ch == 2 ? 0.0f : r);
	};
	if (d.kind == Kind::Depth) {
		const std::string f = b + ".png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) {
			r = g = bl = (px[0] - mn) / range;
		});
		note(f);
	} else if (d.kind == Kind::Ldr) {
		const std::string f = b + ".png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) { chan(px, r, g, bl, 1.f, 0.f); });
		note(f);
	} else if (d.kind == Kind::Hdr) {
		const std::string f = b + ".png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) { chan(px, r, g, bl, 1.f, 0.f); });
		note(f);
		float scale = 1.0f;
		if (ch >= 3) {
			std::vector<float> lum(n);
			for (size_t i = 0; i < n; i++) {
				lum[i] = 0.2126f * d.px[i * 4] + 0.7152f * d.px[i * 4 + 1] + 0.0722f * d.px[i * 4 + 2];
			}
			const size_t k = std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99));
			std::nth_element(lum.begin(), lum.begin() + static_cast<ptrdiff_t>(k), lum.end());
			scale = lum[k] > 1e-6f ? 1.0f / lum[k] : (mx > 1e-6f ? 1.0f / mx : 1.0f);
			const std::string fe = b + "_exp.png";
			WriteMapped(fe, d, [&](const float* px, float& r, float& g, float& bl) {
				r = Gamma(px[0] * scale); g = Gamma(px[1] * scale); bl = Gamma(px[2] * scale);
			});
			note(fe);
		} else {
			const std::string fe = b + "_norm.png";
			WriteMapped(fe, d, [&](const float* px, float& r, float& g, float& bl) {
				chan(px, r, g, bl, 1.0f / range, mn);
			});
			note(fe);
		}
	} else { // Int
		const std::string f = b + "_norm.png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) {
			chan(px, r, g, bl, 1.0f / (mx > 0 ? mx : 1.0f), 0.f);
		});
		note(f);
	}
	if (d.has_alpha && d.kind != Kind::Depth) {
		bool varies = false;
		for (size_t i = 0; i < n && !varies; i++) varies = d.px[i * 4 + 3] < 0.999f;
		if (varies) {
			const std::string f = b + "_alpha.png";
			float amax = 0;
			for (size_t i = 0; i < n; i++) amax = std::max(amax, d.px[i * 4 + 3]);
			const float as = d.kind == Kind::Ldr ? 1.0f : (amax > 0 ? 1.0f / amax : 1.0f);
			WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) {
				r = g = bl = px[3] * as;
			});
			note(f);
		}
	}
	std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u fmt=%s min=%g max=%g ->%s\n", p.seq,
	              p.label.c_str(), Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str(), mn, mx,
	              files.c_str());
	g_log += line;
}

void BarrierImage(vk::CommandBuffer cmd, const Target& t, vk::ImageLayout from, vk::ImageLayout to,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 b {};
	b.srcStageMask                    = src_stage;
	b.srcAccessMask                   = src_access;
	b.dstStageMask                    = dst_stage;
	b.dstAccessMask                   = dst_access;
	b.oldLayout                       = from;
	b.newLayout                       = to;
	b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	b.image                           = t.image;
	b.subresourceRange.aspectMask     = t.full_aspect;
	b.subresourceRange.baseMipLevel   = t.mip;
	b.subresourceRange.levelCount     = 1;
	b.subresourceRange.baseArrayLayer = t.layer;
	b.subresourceRange.layerCount     = 1;
	vk::DependencyInfo dep {};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &b;
	cmd.pipelineBarrier2(dep);
}

// Must be called with g_mutex held. Records the copy into cmd.
void RecordReadback(GraphicContext& gfx, vk::CommandBuffer cmd, const Target& t, const char* label,
                    uint32_t seq) {
	char line[256];
	if (t.w < kMinSize || t.h < kMinSize) return;
	if (t.samples > 1) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u SKIPPED (multisampled)\n", seq,
		              label, Hex(t.addr).c_str(), t.w, t.h);
		g_log += line;
		return;
	}
	if (g_images >= kMaxImages) {
		if (g_images == kMaxImages) {
			g_log += "IMG cap reached (400 images), further images skipped\n";
			g_images++;
		}
		return;
	}
	const uint32_t bpp = BytesPerPixel(t.format);
	if (bpp == 0 || t.image == nullptr) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u fmt=%s SKIPPED (unsupported format)\n",
		              seq, label, Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str());
		g_log += line;
		return;
	}
	const uint64_t size = static_cast<uint64_t>(t.w) * t.h * bpp;
	if (g_pending_bytes + size > kMaxPendingBytes) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u SKIPPED (readback memory cap)\n", seq,
		              label, Hex(t.addr).c_str(), t.w, t.h);
		g_log += line;
		return;
	}
	vk::BufferCreateInfo bi {};
	bi.size  = size;
	bi.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo ai {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer          native = VK_NULL_HANDLE;
	VmaAllocation     alloc  = nullptr;
	VmaAllocationInfo info {};
	if (vmaCreateBuffer(gfx.allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bi), &ai, &native,
	                    &alloc, &info) != VK_SUCCESS) {
		g_log += "IMG buffer allocation failed\n";
		return;
	}
	g_images++;
	g_pending_bytes += size;

	using S = vk::PipelineStageFlagBits2;
	using A = vk::AccessFlagBits2;
	BarrierImage(cmd, t, t.layout, vk::ImageLayout::eTransferSrcOptimal, S::eAllCommands,
	             A::eMemoryWrite, S::eTransfer, A::eTransferRead);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {t.copy_aspect, t.mip, t.layer, 1};
	copy.imageExtent      = vk::Extent3D {t.w, t.h, 1};
	cmd.copyImageToBuffer(t.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(native), 1, &copy);
	BarrierImage(cmd, t, vk::ImageLayout::eTransferSrcOptimal, t.layout, S::eTransfer, A::eTransferRead,
	             S::eAllCommands, A::eMemoryRead | A::eMemoryWrite);

	Pending p;
	p.buffer   = vk::Buffer(native);
	p.alloc    = alloc;
	p.mapped   = info.pMappedData;
	p.size     = size;
	p.target   = t;
	p.seq      = seq;
	p.label    = label;
	p.graphics = &gfx;
	g_pending.push_back(std::move(p));
}

bool MakeTarget(RenderContext& ctx, ImageId id, const ImageViewInfo& view, vk::ImageLayout layout,
                uint64_t addr, Target& out) {
	if (!id) return false;
	auto& image = ctx.GetTextureCache().GetImage(id);
	if (image.backing.image == nullptr) return false;
	out.image   = image.backing.image;
	out.format  = image.backing.format;
	out.depth   = IsDepthFormat(out.format);
	out.full_aspect = out.depth ? (HasStencil(out.format) ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
	                                                      : vk::ImageAspectFlagBits::eDepth)
	                            : vk::ImageAspectFlagBits::eColor;
	out.copy_aspect = out.depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
	out.mip     = view.base_level;
	out.layer   = image.info.IsVolume() ? 0 : view.base_layer;
	out.w       = std::max(image.backing.extent.width >> out.mip, 1u);
	out.h       = std::max(image.backing.extent.height >> out.mip, 1u);
	out.samples = image.backing.samples;
	out.layout  = layout;
	out.addr    = addr;
	return true;
}

std::string DescribeImage(const TextureCache::ImageDesc& d) {
	char b[256];
	std::snprintf(b, sizeof(b), "addr=0x%" PRIx64 " %ux%ux%u guest_fmt=%u vk=%s type=%s mip0=%u layers=%u",
	              d.info.data.address, d.info.extent.width, d.info.extent.height, d.info.extent.depth,
	              static_cast<unsigned>(d.info.guest_format), vk::to_string(d.info.pixel_format).c_str(),
	              TypeName(d.info.type), d.view_info.base_level, d.info.resources.layers);
	return b;
}

void AppendImages(std::string& s, const char* tag, const std::vector<TextureBinding>* images) {
	if (images == nullptr) return;
	for (size_t i = 0; i < images->size(); i++) {
		const auto& im = (*images)[i];
		s += "    ";
		s += tag;
		s += "[" + std::to_string(i) + "]: " + DescribeImage(im.desc) +
		     (im.desc.type == TextureCache::BindingType::Storage ? " STORAGE" : "") + "\n";
	}
}

void BeginCapture(CommandBuffer& buffer) {
	buffer.EndRendering();
	std::error_code ec;
	const char* root_env = std::getenv("KYTY_DBG_FRAME_DUMP_DIR");
	const std::string root = (root_env != nullptr && *root_env != '\0') ? root_env : "F:/emuf/captures";
	for (;; g_frame++) {
		const auto p = std::filesystem::path(root) / ("frame_" + std::to_string(g_frame));
		if (!std::filesystem::exists(p, ec)) {
			g_dir = p.generic_string();
			break;
		}
	}
	std::filesystem::create_directories(g_dir, ec);
	g_targets.clear();
	g_pending.clear();
	g_log.clear();
	g_seq = 0;
	g_images = 0;
	g_pending_bytes = 0;
	g_capturing = true;
	g_active.store(true, std::memory_order_relaxed);
	std::printf("NHL27FRAMEDUMP: capturing frame -> %s\n", g_dir.c_str());
	std::fflush(stdout);
}

void FinishCapture(CommandBuffer& buffer) {
	buffer.GetContext().GetCommandScheduler().FlushAndWait();
	g_active.store(false, std::memory_order_relaxed);
	g_capturing = false;
	std::string draws = g_log;
	g_log.clear();
	const uint32_t n = static_cast<uint32_t>(g_pending.size());
	for (auto& p : g_pending) {
		ProcessOne(p);
		vmaDestroyBuffer(p.graphics->allocator, static_cast<VkBuffer>(p.buffer), p.alloc);
	}
	g_pending.clear();
	g_targets.clear();
	std::string out = "# KytyPS5 frame dump (KYTY_DBG_FRAME_DUMP). Sequence = draw/dispatch order in the frame.\n"
	                  "# Image files are named <seq>_<rt<slot>|depth|st<slot>>_<guest addr>_<WxH>_<vk format>.png\n"
	                  "# HDR formats: <name>.png = clamped 0..1 (linear), <name>_exp.png = auto-exposed (p99 luminance) + sRGB.\n\n";
	out += draws;
	out += "\n# ---- images ----\n";
	out += g_log;
	if (FILE* f = std::fopen((g_dir + "/frame.txt").c_str(), "wb")) {
		std::fwrite(out.data(), 1, out.size(), f);
		std::fclose(f);
	}
	std::printf("NHL27FRAMEDUMP: done, %u draws/dispatches, %u images -> %s\n", g_seq, n, g_dir.c_str());
	std::fflush(stdout);
	g_log.clear();
}

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_DBG_FRAME_DUMP");
		return v != nullptr && *v != '\0' && *v != '0';
	}();
	return enabled;
}

void Request() {
	if (!Enabled()) return;
	g_request.store(true);
	std::printf("NHL27FRAMEDUMP: F2 - will capture the next complete frame\n");
	std::fflush(stdout);
}

void OnGuestFlip(CommandBuffer& buffer) {
	if (!Enabled()) return;
	std::lock_guard lock(g_mutex);
	if (g_capturing) {
		FinishCapture(buffer);
	} else if (g_request.exchange(false)) {
		BeginCapture(buffer);
	}
}

void OnDraw(RenderContext& context, const DrawLog& draw) {
	std::lock_guard lock(g_mutex);
	if (!g_capturing) return;
	const uint32_t seq = g_seq++;
	char b[512];
	std::snprintf(b, sizeof(b),
	              "#%05u draw vs=0x%016" PRIx64 " ps=0x%016" PRIx64 " %s%s count=%u instances=%u ps_active=%d "
	              "ztest=%d zwrite=%d zfunc=%s\n",
	              seq, draw.vs_hash, draw.ps_hash, draw.indexed ? "indexed" : "auto",
	              draw.indirect ? " INDIRECT" : (draw.mesh ? " MESH" : ""), draw.count, draw.instances,
	              draw.ps_active ? 1 : 0, draw.depth_test ? 1 : 0, draw.depth_write ? 1 : 0,
	              vk::to_string(draw.depth_compare).c_str());
	std::string s = b;
	for (const auto& c : draw.colors) {
		if (c.desc == nullptr) continue;
		s += "    rt" + std::to_string(c.slot) + ": " + DescribeImage(*c.desc) + " target_mip=" +
		     std::to_string(c.mip) + " target_layer=" + std::to_string(c.layer) + " extent=" +
		     std::to_string(c.extent.width) + "x" + std::to_string(c.extent.height) + "\n";
		Target t;
		if (c.view && MakeTarget(context, c.image_id, c.desc->view_info, c.layout, c.desc->info.data.address, t)) {
			t.slot = c.slot;
			t.seq  = seq;
			t.dirty = true;
			g_targets[reinterpret_cast<uint64_t>(static_cast<VkImageView>(c.view))] = t;
		}
	}
	if (draw.depth != nullptr && draw.depth->desc != nullptr) {
		const auto& c = *draw.depth;
		s += "    depth: " + DescribeImage(*c.desc) + "\n";
		Target t;
		if (c.view && MakeTarget(context, c.image_id, c.desc->view_info, c.layout, c.desc->info.data.address, t)) {
			t.seq   = seq;
			t.dirty = true;
			g_targets[reinterpret_cast<uint64_t>(static_cast<VkImageView>(c.view))] = t;
		}
	}
	for (int i = 0; i < 3; i++) AppendImages(s, ("vs_img" + std::to_string(i)).c_str(), draw.vs_images[i]);
	AppendImages(s, "ps_img", draw.ps_images);
	g_log += s;
}

void OnEndRendering(const CommandBuffer& buffer, const RenderState& pass) {
	std::lock_guard lock(g_mutex);
	if (!g_capturing) return;
	auto&      gfx = buffer.GetGraphics();
	const auto cmd = buffer.Handle();
	const auto dump = [&](vk::ImageView view, const char* label_prefix, bool is_depth) {
		if (!view) return;
		const auto it = g_targets.find(reinterpret_cast<uint64_t>(static_cast<VkImageView>(view)));
		if (it == g_targets.end() || !it->second.dirty) return;
		it->second.dirty = false;
		std::string label = label_prefix;
		if (!is_depth) label += std::to_string(it->second.slot);
		RecordReadback(gfx, cmd, it->second, label.c_str(), it->second.seq);
	};
	for (uint32_t i = 0; i < pass.num_color_attachments; i++) {
		dump(pass.color_attachments[i].image_view, "rt", false);
	}
	if (pass.depth_stencil_attachment.has_depth || pass.depth_stencil_attachment.has_stencil) {
		dump(pass.depth_stencil_attachment.image_view, "depth", true);
	}
}

void OnDispatch(RenderContext& context, CommandBuffer& buffer, uint64_t cs_hash, uint32_t gx,
                uint32_t gy, uint32_t gz, bool indirect, std::span<const StorageUse> images) {
	std::lock_guard lock(g_mutex);
	if (!g_capturing) return;
	const uint32_t seq = g_seq++;
	char b[256];
	std::snprintf(b, sizeof(b), "#%05u dispatch cs=0x%016" PRIx64 " groups=%ux%ux%u%s\n", seq, cs_hash, gx,
	              gy, gz, indirect ? " INDIRECT" : "");
	std::string s = b;
	for (size_t i = 0; i < images.size(); i++) {
		const auto& u = images[i];
		if (u.binding == nullptr) continue;
		s += "    img[" + std::to_string(i) + "]: " + DescribeImage(u.binding->desc) +
		     (u.storage ? " STORAGE" : " sampled") + (u.written ? " WRITTEN" : "") + "\n";
	}
	g_log += s;
	for (size_t i = 0; i < images.size(); i++) {
		const auto& u = images[i];
		if (u.binding == nullptr || !u.written || !u.storage) continue;
		Target t;
		if (!MakeTarget(context, u.binding->image_id, u.binding->desc.view_info, u.binding->layout,
		                u.binding->desc.info.data.address, t)) {
			continue;
		}
		t.slot = static_cast<uint32_t>(i);
		const std::string label = "st" + std::to_string(i);
		RecordReadback(buffer.GetGraphics(), buffer.Handle(), t, label.c_str(), seq);
	}
}

} // namespace Libs::Graphics::FrameDump
