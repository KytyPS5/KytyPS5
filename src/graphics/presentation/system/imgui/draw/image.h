#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_IMAGE_H_

#include "graphics/presentation/system/common/props/imageStyles.h"
#include "graphics/presentation/system/core/geometry.h"

struct ImDrawList;
struct ImTextureData;

namespace Libs::Graphics::SystemUi {

/// Where DrawImage puts an image of `size` pixels given `rect` and `fit`. A contained image is the
/// picture alone, with the empty bars left out, and the others fill `rect`.
[[nodiscard]] Rect PlacedImageRect(Vec2 size, const Rect& rect, ImageFit fit);

/// Draws `texture`, which is `size` pixels, in `rect`, fitted and with its corners rounded as
/// `styles` says, at the current scale. `opacity` fades it with its element. A null texture draws
/// nothing. It needs an open ContextScope.
void DrawImage(ImDrawList& list, ImTextureData* texture, Vec2 size, const Rect& rect,
               const ImageStyles& styles, float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_IMAGE_H_
