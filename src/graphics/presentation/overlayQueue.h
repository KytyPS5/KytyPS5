#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_OVERLAYQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_OVERLAYQUEUE_H_

// Whether the system overlay draws with the queue idle. The ImGui backend destroys at once a
// vertex or index buffer that a draw outgrows, and a texture the overlay retired. Each command
// buffer committed while that object lived holds it (see queueCommits.h), so the queue is
// drained before such a draw.

namespace Libs::Graphics {

struct OverlayDraw {
	int  vertices         = 0;
	int  indices          = 0;
	// A texture the overlay retired is destroyed during this draw.
	bool destroys_texture = false;
};

// The sizes the backend's buffer set for this draw has grown to.
struct OverlayBuffers {
	int vertices = 0;
	int indices  = 0;
};

[[nodiscard]] inline bool OverlayDrawNeedsIdleQueue(const OverlayDraw&    draw,
                                                    const OverlayBuffers& buffers) {
	return draw.destroys_texture ||
	       (draw.vertices > 0 && (draw.vertices > buffers.vertices || draw.indices > buffers.indices));
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_PRESENTATION_OVERLAYQUEUE_H_ */
