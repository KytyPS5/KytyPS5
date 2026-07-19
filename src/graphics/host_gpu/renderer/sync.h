#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNC_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNC_H_

#include "kernel/eventQueue.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;

namespace Sync {

[[nodiscard]] bool     ScaleReferenceClock(uint64_t host_ticks, uint64_t host_frequency,
                                           uint64_t* value);
[[nodiscard]] uint64_t ReadReferenceClock();

void TriggerAgcUserInterrupt();
void TriggerEopEvent(uint32_t context_id);
void TriggerEopEventAtEndOfPipe(CommandBuffer* buffer, uint32_t context_id);

void WriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                        uint32_t value);
void WriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                        uint64_t value);
void WriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                           uint32_t dw_offset, uint32_t dw_num);
void WriteAtEndOfPipeClockCounter(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                  uint64_t value);
void WriteAtEndOfPipeClockCounterWithWriteBack(uint64_t submit_id, CommandBuffer* buffer,
                                               uint64_t* dst_gpu_addr, uint64_t value);
void WriteAtEndOfPipeWithWriteBack32(uint64_t submit_id, CommandBuffer* buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value);
void WriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer* buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value);
void WriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value,
                                     uint32_t context_id = 0);
void WriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value,
                                     uint32_t context_id = 0);
void WriteAtEndOfPipeWithInterruptWriteBack32(uint64_t submit_id, CommandBuffer* buffer,
                                              uint32_t* dst_gpu_addr, uint32_t value,
                                              uint32_t context_id = 0);
void WriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer* buffer,
                                              uint64_t* dst_gpu_addr, uint64_t value,
                                              uint32_t context_id = 0);

[[nodiscard]] uint64_t PrepareDisplayBufferFlip(CommandBuffer* buffer, int handle, int index,
                                                int flip_mode, int64_t flip_arg);
void WriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer* buffer, int handle, int index,
                              int flip_mode, int64_t flip_arg, uint64_t request_id);
void WriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                uint32_t value, int handle, int index, int flip_mode,
                                int64_t flip_arg, uint64_t request_id);
void WriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer* buffer,
                                                  uint32_t* dst_gpu_addr, uint32_t value,
                                                  int handle, int index, int flip_mode,
                                                  int64_t flip_arg, uint64_t request_id);

int  AddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata);
int  DeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id);
void ReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);
void DeleteBuffers();

} // namespace Sync
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNC_H_
