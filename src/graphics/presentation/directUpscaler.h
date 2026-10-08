#ifndef KYTY_GRAPHICS_PRESENTATION_DIRECT_UPSCALER_H_
#define KYTY_GRAPHICS_PRESENTATION_DIRECT_UPSCALER_H_

#include "graphics/presentation/dlss.h"

#include <memory>
#include <optional>
#include <vector>

namespace Libs::Graphics {

// Calls the package's XeSS/FidelityFX Vulkan runtimes without NGX translation.
class DirectUpscaler final {
public:
	[[nodiscard]] static bool Selected();
	// Before Vulkan objects: the runtime's instance and device extensions.
	static bool AppendInstanceExtensions(std::vector<const char*>&                   enabled,
	                                     const std::vector<vk::ExtensionProperties>& available);
	static bool AppendDeviceExtensions(GraphicContext& graphics, std::vector<const char*>& enabled,
	                                   const std::vector<vk::ExtensionProperties>& available);
	// Enables the runtime's required device features in these creation structures and
	// prepends any other feature structure to `next`. False when the GPU lacks one.
	static bool
	EnableDeviceFeatures(GraphicContext& graphics, vk::PhysicalDeviceFeatures& features,
	                     vk::PhysicalDeviceVulkan11Features&                 features11,
	                     vk::PhysicalDeviceVulkan12Features&                 features12,
	                     vk::PhysicalDeviceVulkan13Features&                 features13,
	                     vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT& mutable_descriptor,
	                     const void*&                                        next);

	DirectUpscaler(GraphicContext& graphics, CommandScheduler& scheduler);
	~DirectUpscaler();
	KYTY_CLASS_NO_COPY(DirectUpscaler);

	[[nodiscard]] bool                        Available() const;
	[[nodiscard]] const char*                 Name() const;
	[[nodiscard]] std::optional<vk::Extent2D> OptimalInputExtent(vk::Extent2D output,
	                                                             vk::Extent2D source) const;
	// Same contract as DlssProcessor::Evaluate.
	[[nodiscard]] bool Evaluate(CommandBuffer& command, const DlssFrameInputs& inputs,
	                            VulkanImage& output, vk::ImageView output_view);
	void               SkipFrame();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif
