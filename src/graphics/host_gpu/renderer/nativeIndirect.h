#ifndef GRAPHICS_HOST_GPU_RENDERER_NATIVEINDIRECT_H
#define GRAPHICS_HOST_GPU_RENDERER_NATIVEINDIRECT_H
#include "graphics/host_gpu/vulkanCommon.h"
namespace Libs::Graphics {
struct GraphicContext;
class CommandBuffer;
struct NativeIndirectDraw;
// Mirrors the CP's indexed-count clamp without downloading its argument record.
class NativeIndirectPrep {
public:
 explicit NativeIndirectPrep(GraphicContext& graphics);
 ~NativeIndirectPrep();
 void Record(CommandBuffer& buffer, const NativeIndirectDraw& source);
private:
 GraphicContext& m_graphics;
 vk::PipelineLayout m_layout = nullptr;
 vk::Pipeline m_pipeline = nullptr;
};
}
#endif