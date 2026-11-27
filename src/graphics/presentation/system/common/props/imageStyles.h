#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_IMAGE_STYLES_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_IMAGE_STYLES_H_

#include "graphics/presentation/system/core/units.h"

#include <cstdint>

namespace Libs::Graphics::SystemUi {

/// How an image fits a rectangle of a different shape.
enum class ImageFit : uint8_t {
	Fill,    // stretched to the rectangle
	Contain, // whole, centred, with space left on two sides
	Cover,   // filling the rectangle, centred, with the excess cropped
};

/// How an image looks in its space, apart from which image it is. The drawing layer and the Image
/// element both take it whole, so the two can't drift apart.
struct ImageStyles {
	ImageFit fit = ImageFit::Contain;
	// How far its corners are rounded, in design units, never past half its shorter side.
	Dp radius;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_IMAGE_STYLES_H_
