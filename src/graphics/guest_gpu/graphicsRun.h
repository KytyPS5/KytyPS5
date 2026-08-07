#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/uniqueFunction.h"

#include <memory>

namespace Libs::Graphics {

class GpuState;
class RenderContext;

class Gpu final {
public:
	explicit Gpu(RenderContext& renderer);
	~Gpu();
	KYTY_CLASS_NO_COPY(Gpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);

	void Submit(uint32_t* draw_commands, uint32_t draw_size_dw, uint32_t* constant_commands,
	            uint32_t constant_size_dw, bool trigger_agc_interrupt_on_done = false);
	void SubmitCompute(uint32_t queue, uint32_t* commands, uint32_t size_dw,
	                   bool trigger_agc_interrupt_on_done = false);
	void SubmitFlipPreparation(uint64_t request_id);
	void SuspendPoint();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;

private:
	std::unique_ptr<GpuState> m_state;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
