#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_ENCODED_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_ENCODED_IMAGE_H_

#include <cstddef>
#include <memory>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// An image still encoded as PNG or JPEG, such as a trophy's icon. Shared, so each copy of whatever
/// carries it doesn't copy the bytes, and its address tells one image from another, so an image
/// cache can decode it once however many elements show it.
using EncodedImage = std::shared_ptr<const std::vector<std::byte>>;

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_ENCODED_IMAGE_H_
