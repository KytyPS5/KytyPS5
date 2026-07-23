#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/render.h"

#include <array>
#include <memory>

namespace Libs::Graphics {

class CommandScheduler {
public:
	static constexpr int BufferCount = 8;

	CommandScheduler()  = default;
	~CommandScheduler() = default;
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           Flush();
	CommandBuffer& FlushAndGetSubmitted();
	void           Finish();
	void           FinishCurrent();

	[[nodiscard]] bool   Active() const noexcept { return m_current >= 0; }
	void                 CheckActive() const;
	RenderCommandBuffer& Current() const;

private:
	void           BindCurrent() const;
	CommandBuffer& SubmitCurrent();
	void           BeginNext();

	std::array<std::unique_ptr<RenderCommandBuffer>, BufferCount> m_buffers;
	int                                                           m_current     = -1;
	bool                                                          m_recording   = false;
	HW::Context*                                                  m_registers   = nullptr;
	HW::UserConfig*                                               m_user_config = nullptr;
	HW::Shader*                                                   m_shaders     = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
