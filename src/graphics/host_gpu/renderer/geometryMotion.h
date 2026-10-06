#ifndef KYTY_GRAPHICS_GEOMETRY_MOTION_H_
#define KYTY_GRAPHICS_GEOMETRY_MOTION_H_
#include "graphics/host_gpu/renderer/renderTarget.h"
#include <memory>
#include <span>
#include <array>

namespace Libs::Graphics {
struct GraphicContext;
class CommandScheduler;
class CommandBuffer;
// Current/previous guest shader positions. No optical flow, camera-matrix
// guessing, game identifiers or guest shader replays are used here.
class GeometryMotion {
public:
	GeometryMotion(GraphicContext& graphics, CommandScheduler& scheduler);
	~GeometryMotion();
	GeometryMotion(const GeometryMotion&) = delete;
	GeometryMotion& operator=(const GeometryMotion&) = delete;
	std::array<uint32_t, 14> PrepareDraw(CommandBuffer& command, std::span<const uint64_t> key,
	                                  uint32_t capacity, uint32_t first_instance, uint32_t instances);
	bool Attach(CommandBuffer& command, RenderState& state);
	// Call right after BeginRendering for an attached draw; clears a reset guide in-pass.
	void BeginPass(CommandBuffer& command);
	bool SupportsSurface(const Image& color, vk::Extent2D extent) const;
	void EndPass(const RenderState& state);
	Image* Source(const Image& color) const;
	void AdvanceFrame();
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics
#endif
