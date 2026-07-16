#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

struct Label;
class CommandBuffer;
struct GraphicContext;

void LabelInit();

constexpr int LABEL_ARGS_MAX = 5;
using LabelCallback          = bool (*)(const uint64_t* args);

Label* LabelCreate64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value,
                     LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args);
Label* LabelCreate32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value,
                     LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args);
void   LabelDelete(Label* label);
void   LabelSet(CommandBuffer* buffer, Label* label);
void   LabelDrain();
void   LabelWriteGuestMemory(void* address, const void* data, uint64_t size);
[[nodiscard]] bool LabelInCallback() noexcept;

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_ */
