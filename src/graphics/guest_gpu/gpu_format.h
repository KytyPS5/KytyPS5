#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_FORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_FORMAT_H_

#include "common/common.h"

namespace Libs::Graphics::Prospero {

uint32_t NumBytesPerElement(uint32_t format);
uint32_t BlockCompressedBytesPerBlock(uint32_t format);
uint32_t RenderTargetBytesPerElement(uint32_t format);
bool     IsSupportedTextureFormat(uint32_t format);
bool     IsUintTextureFormat(uint32_t format);
bool     IsFmaskTextureFormat(uint32_t format);

} // namespace Libs::Graphics::Prospero

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_FORMAT_H_ */
