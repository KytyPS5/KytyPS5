#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_PRESENTATION_FRAME_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_PRESENTATION_FRAME_H_
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/dlssFrameGeneration.h"
#include "graphics/presentation/frameTiming.h"
#include "graphics/presentation/presenter.h"
namespace Libs::Graphics {
class Image;
// Internal prepared-frame ownership shared by the presenter and GPU checks.
struct Presenter::Frame {
	VulkanImage   image;
	vk::ImageView view           = nullptr;
	uint64_t      present_tick   = 0;
	uint64_t      producer_tick  = 0;
	bool          busy           = false;
	bool          dlss_evaluated = false;
	bool          dlss_bypassed = false; // SR skipped because the source already covers the output.
	bool          guest_frame   = false;
	FramePreparationTiming        preparation;
	std::unique_ptr<DlssFgInputs> fg_inputs;
	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format,
	               bool storage = false);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};
} // namespace Libs::Graphics
#endif
