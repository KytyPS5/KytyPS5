#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDIMAGERECORD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDIMAGERECORD_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/image.h"

#include <variant>

namespace Libs::Graphics {

struct VulkanImage;

class CachedImageRecord final {
public:
	enum class Kind : uint8_t { Texture, StorageTexture, RenderTarget, DepthTarget, VideoOut };

	CachedImageRecord(Kind kind, const ImageInfo& info);
	explicit CachedImageRecord(const RenderTargetInfo& info);
	explicit CachedImageRecord(const DepthTargetInfo& info);
	explicit CachedImageRecord(const VideoOutInfo& info);
	~CachedImageRecord();
	KYTY_CLASS_NO_COPY(CachedImageRecord);

	[[nodiscard]] Image&                 ImageDesc();
	[[nodiscard]] const Image&           ImageDesc() const;
	[[nodiscard]] RenderTargetInfo&      RenderTargetDesc();
	[[nodiscard]] const RenderTargetInfo& RenderTargetDesc() const;
	[[nodiscard]] DepthTargetInfo&       DepthTargetDesc();
	[[nodiscard]] const DepthTargetInfo& DepthTargetDesc() const;
	[[nodiscard]] VideoOutInfo&          VideoOutDesc();
	[[nodiscard]] const VideoOutInfo&    VideoOutDesc() const;

	[[nodiscard]] uint32_t           RangeCount() const;
	[[nodiscard]] BufferImageBinding BufferBinding() const;
	[[nodiscard]] uint64_t           Address(uint32_t index = 0) const;
	[[nodiscard]] uint64_t           Size(uint32_t index = 0) const;
	[[nodiscard]] bool               OverlapsRange(uint64_t address, uint64_t size, bool page) const;
	[[nodiscard]] bool IsGpuReadbackPageCandidate(uint64_t address, uint64_t size) const;
	[[nodiscard]] bool HasExactRange(uint64_t address, uint64_t size) const;

	const Kind   kind;
	VulkanImage* image               = nullptr;
	bool         gpu_modified        = false;
	bool         buffer_modified     = false;
	bool         stencil_initialized = false;
	bool         registered          = false;

private:
	[[nodiscard]] static Image MakeImage(const ImageInfo& info);
	[[nodiscard]] bool         DescriptorMatchesKind() const;

	std::variant<Image, RenderTargetInfo, DepthTargetInfo, VideoOutInfo> m_descriptor;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDIMAGERECORD_H_
