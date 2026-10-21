#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_NULLTEXTURES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_NULLTEXTURES_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>

namespace Libs::Graphics {

// What a null guest texture is bound as where the device has no null descriptor
// (VK_EXT_robustness2's nullDescriptor; MoltenVK). A null texture on the guest reads zero in
// every component and drops writes. Each image binding kind gets one zero-filled 1x1 image of
// its view type, sample count and numeric class, made at start and kept in the general layout.
// A sampled placeholder is never written, so it always reads zero. Writes to a null storage
// texture land in that kind's storage placeholder, which is cleared again before each draw or
// dispatch that binds it (Reset), so no write is seen by a later one.
class NullTextures {
public:
	explicit NullTextures(GraphicContext& graphics);
	~NullTextures();
	KYTY_CLASS_NO_COPY(NullTextures);

	// The placeholder for a null texture bound at `kind`. Stops the emulator where the device has
	// no placeholder of that kind.
	[[nodiscard]] vk::DescriptorImageInfo ImageInfo(ShaderRecompiler::IR::DescriptorBindingKind kind) const;
	// Clears the storage placeholder of `kind` to zero, outside rendering, ahead of the commands
	// that bind it.
	void Reset(ShaderRecompiler::IR::DescriptorBindingKind kind, vk::CommandBuffer command) const;

private:
	struct Placeholder {
		VulkanImage   image;
		vk::ImageView view = nullptr;
	};

	void Create();

	GraphicContext&                                                     m_graphics;
	std::array<Placeholder, ShaderRecompiler::IR::ImageBindingCount> m_placeholders {};
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_NULLTEXTURES_H_ */
