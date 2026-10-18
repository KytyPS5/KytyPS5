#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_DDSIMAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_DDSIMAGE_H_

#include "graphics/host_gpu/graphicContext.h"
#include "imgui.h"

#include <filesystem>

namespace Libs::Graphics {

class DdsImage final {
public:
	explicit DdsImage(GraphicContext& graphics): m_graphics(graphics) {}
	~DdsImage();
	KYTY_CLASS_NO_COPY(DdsImage);

	bool                       Load(const std::filesystem::path& path);
	void                       Upload(vk::CommandBuffer command);
	[[nodiscard]] ImTextureRef Texture() const;

private:
	GraphicContext& m_graphics;
	VulkanImage     m_image;
	vk::ImageView   m_view               = nullptr;
	VkDescriptorSet m_descriptor         = VK_NULL_HANDLE;
	VkBuffer        m_staging            = VK_NULL_HANDLE;
	VmaAllocation   m_staging_allocation = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_DDSIMAGE_H_
