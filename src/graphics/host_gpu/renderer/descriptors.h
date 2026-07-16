#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>

namespace Libs::Graphics {

struct VulkanImage;

[[nodiscard]] bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor,
                                                    const VulkanImage&           image);
void ValidateMetadataReuseTexture(const ShaderRecompiler::IR::ImageResource& resource,
                                  const ShaderTextureResource& descriptor, uint64_t size);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
