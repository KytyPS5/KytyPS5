#include "graphics/host_gpu/renderer/cachedImageRecord.h"

#include "common/assert.h"

namespace Libs::Graphics {

CachedImageRecord::CachedImageRecord(Kind kind, const ImageInfo& info)
    : kind(kind), m_descriptor(MakeImage(info)) {
	if (kind != Kind::Texture && kind != Kind::StorageTexture) {
		EXIT("TextureCache: image descriptor has incompatible kind %u\n",
		     static_cast<uint32_t>(kind));
	}
}

CachedImageRecord::CachedImageRecord(const RenderTargetInfo& info)
    : kind(Kind::RenderTarget), m_descriptor(info) {}

CachedImageRecord::CachedImageRecord(const DepthTargetInfo& info)
    : kind(Kind::DepthTarget), m_descriptor(info) {}

CachedImageRecord::CachedImageRecord(const VideoOutInfo& info)
    : kind(Kind::VideoOut), m_descriptor(info) {}

CachedImageRecord::~CachedImageRecord() {
	if (image == nullptr || registered || !DescriptorMatchesKind()) {
		EXIT("TextureCache: cached image destroyed with invalid resources, image=%p "
		     "kind=%u registered=%d\n",
		     static_cast<const void*>(image), static_cast<uint32_t>(kind), registered);
	}
	ImageOps::Destroy(*image);
	image = nullptr;
}

Image& CachedImageRecord::ImageDesc() {
	return std::get<Image>(m_descriptor);
}

const Image& CachedImageRecord::ImageDesc() const {
	return std::get<Image>(m_descriptor);
}

RenderTargetInfo& CachedImageRecord::RenderTargetDesc() {
	return std::get<RenderTargetInfo>(m_descriptor);
}

const RenderTargetInfo& CachedImageRecord::RenderTargetDesc() const {
	return std::get<RenderTargetInfo>(m_descriptor);
}

DepthTargetInfo& CachedImageRecord::DepthTargetDesc() {
	return std::get<DepthTargetInfo>(m_descriptor);
}

const DepthTargetInfo& CachedImageRecord::DepthTargetDesc() const {
	return std::get<DepthTargetInfo>(m_descriptor);
}

VideoOutInfo& CachedImageRecord::VideoOutDesc() {
	return std::get<VideoOutInfo>(m_descriptor);
}

const VideoOutInfo& CachedImageRecord::VideoOutDesc() const {
	return std::get<VideoOutInfo>(m_descriptor);
}

uint32_t CachedImageRecord::RangeCount() const {
	return kind == Kind::DepthTarget && DepthTargetDesc().stencil_address != 0 ? 2u : 1u;
}

BufferImageBinding CachedImageRecord::BufferBinding() const {
	switch (kind) {
		case Kind::Texture: return BufferImageBinding::Texture;
		case Kind::VideoOut: return BufferImageBinding::VideoOut;
		case Kind::RenderTarget: return BufferImageBinding::RenderTarget;
		case Kind::StorageTexture: return BufferImageBinding::StorageTexture;
		case Kind::DepthTarget: return BufferImageBinding::DepthTarget;
	}
	return BufferImageBinding::Unsupported;
}

uint64_t CachedImageRecord::Address(uint32_t index) const {
	if (index >= RangeCount()) {
		EXIT("TextureCache: image address range index out of bounds, index=%u count=%u\n", index,
		     RangeCount());
	}
	if (index == 1) {
		return DepthTargetDesc().stencil_address;
	}
	switch (kind) {
		case Kind::Texture:
		case Kind::StorageTexture: return ImageDesc().address;
		case Kind::RenderTarget: return RenderTargetDesc().address;
		case Kind::DepthTarget: return DepthTargetDesc().address;
		case Kind::VideoOut: return VideoOutDesc().address;
	}
	EXIT("TextureCache: unsupported cached image kind %u for address\n",
	     static_cast<uint32_t>(kind));
}

uint64_t CachedImageRecord::Size(uint32_t index) const {
	if (index >= RangeCount()) {
		EXIT("TextureCache: image size range index out of bounds, index=%u count=%u\n", index,
		     RangeCount());
	}
	if (index == 1) {
		return DepthTargetDesc().stencil_size;
	}
	switch (kind) {
		case Kind::Texture:
		case Kind::StorageTexture: return ImageDesc().size;
		case Kind::RenderTarget: return RenderTargetDesc().size;
		case Kind::DepthTarget: return DepthTargetDesc().size;
		case Kind::VideoOut: return VideoOutDesc().size;
	}
	EXIT("TextureCache: unsupported cached image kind %u for size\n", static_cast<uint32_t>(kind));
}

bool CachedImageRecord::OverlapsRange(uint64_t address, uint64_t size, bool page) const {
	for (uint32_t i = 0; i < RangeCount(); i++) {
		if (page ? ImagePageRangesOverlap(address, size, Address(i), Size(i))
		         : ImageRangeOverlaps(address, size, Address(i), Size(i))) {
			return true;
		}
	}
	return false;
}

bool CachedImageRecord::IsGpuReadbackPageCandidate(uint64_t address, uint64_t size) const {
	return gpu_modified && OverlapsRange(address, size, true);
}

bool CachedImageRecord::HasExactRange(uint64_t address, uint64_t size) const {
	for (uint32_t i = 0; i < RangeCount(); i++) {
		if (address == Address(i) && size == Size(i)) {
			return true;
		}
	}
	return false;
}

Image CachedImageRecord::MakeImage(const ImageInfo& info) {
	Image image;
	image = info;
	return image;
}

bool CachedImageRecord::DescriptorMatchesKind() const {
	switch (kind) {
		case Kind::Texture:
		case Kind::StorageTexture: return std::holds_alternative<Image>(m_descriptor);
		case Kind::RenderTarget: return std::holds_alternative<RenderTargetInfo>(m_descriptor);
		case Kind::DepthTarget: return std::holds_alternative<DepthTargetInfo>(m_descriptor);
		case Kind::VideoOut: return std::holds_alternative<VideoOutInfo>(m_descriptor);
	}
	return false;
}

} // namespace Libs::Graphics
