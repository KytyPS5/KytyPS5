#ifndef KYTY_GRAPHICS_REGRESSION_FRAMEREGRESSIONVULKAN_H_
#define KYTY_GRAPHICS_REGRESSION_FRAMEREGRESSIONVULKAN_H_

#include "common/common.h"

namespace Libs::Graphics {

struct VulkanImage;

namespace Regression {

// These functions form the only Vulkan-facing edge of the CPU-testable regression module.
[[nodiscard]] bool InitializeFrameRegression();
[[nodiscard]] bool ObservePresentedFrame(VulkanImage& image);
[[nodiscard]] int  FinalizeFrameRegression();

} // namespace Regression
} // namespace Libs::Graphics

#endif // KYTY_GRAPHICS_REGRESSION_FRAMEREGRESSIONVULKAN_H_
