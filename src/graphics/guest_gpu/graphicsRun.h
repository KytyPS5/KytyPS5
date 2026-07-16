#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class GraphicsRunSubmissionLock final {
public:
	GraphicsRunSubmissionLock();
	~GraphicsRunSubmissionLock();
	KYTY_CLASS_NO_COPY(GraphicsRunSubmissionLock);
};

void GraphicsRunInit();

void GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
                       uint32_t num_const_dw, bool trigger_agc_interrupt_on_done = false);
void GraphicsRunSubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                              bool trigger_agc_interrupt_on_done = false);
void GraphicsRunWait();
void GraphicsRunDone();
int  GraphicsRunGetFrameNum();
[[nodiscard]] bool GraphicsRunIsCommandProcessorThread() noexcept;
[[nodiscard]] bool GraphicsRunIsGuestMemoryPoll() noexcept;
[[nodiscard]] bool GraphicsRunIsCommandProcessorGuestAccess() noexcept;
[[nodiscard]] bool GraphicsRunSubmissionLockHeld() noexcept;
[[nodiscard]] bool GraphicsRunGpuLockHeld() noexcept;
// Begin/end an exclusive same-command-processor readback transaction. The begin call finishes
// work already recorded on that queue and retains the processor lock so its peer draw/constant
// job cannot record new work until BufferCache publishes the faulted page.
[[nodiscard]] int GraphicsRunBeginCommandProcessorReadback();
void              GraphicsRunEndCommandProcessorReadback();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
