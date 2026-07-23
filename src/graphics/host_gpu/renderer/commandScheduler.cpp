#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"

namespace Libs::Graphics {

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	m_registers   = &registers;
	m_user_config = &user_config;
	m_shaders     = &shaders;

	if (!Active()) {
		for (auto& buffer: m_buffers) {
			buffer = std::make_unique<RenderCommandBuffer>();
		}
		m_current = 0;
	}

	BindCurrent();
	if (!m_recording) {
		Current().Begin();
		m_recording = true;
	}
}

void CommandScheduler::Flush() {
	SubmitCurrent();
	BeginNext();
}

CommandBuffer& CommandScheduler::FlushAndGetSubmitted() {
	auto& submitted = SubmitCurrent();
	BeginNext();
	return submitted;
}

void CommandScheduler::Finish() {
	CheckActive();
	if (m_recording) {
		SubmitCurrent();
	}
	for (auto& buffer: m_buffers) {
		buffer->WaitForFenceAndReset();
	}
	BindCurrent();
	Current().Begin();
	m_recording = true;
}

void CommandScheduler::FinishCurrent() {
	auto& submitted = SubmitCurrent();
	submitted.WaitForFenceAndReset();
	submitted.Begin();
	m_recording = true;
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active() || m_current >= BufferCount);
}

RenderCommandBuffer& CommandScheduler::Current() const {
	CheckActive();
	EXIT_IF(m_buffers[m_current] == nullptr);
	return *m_buffers[m_current];
}

void CommandScheduler::BindCurrent() const {
	EXIT_IF(m_registers == nullptr || m_user_config == nullptr || m_shaders == nullptr);
	Current().Bind(*m_registers, *m_user_config, *m_shaders);
}

CommandBuffer& CommandScheduler::SubmitCurrent() {
	CheckActive();
	EXIT_IF(!m_recording);
	auto& submitted = Current();
	submitted.End();
	submitted.Execute();
	m_recording = false;
	return submitted;
}

void CommandScheduler::BeginNext() {
	EXIT_IF(m_recording);
	m_current = (m_current + 1) % BufferCount;
	Current().WaitForFenceAndReset();
	BindCurrent();
	Current().Begin();
	m_recording = true;
}

} // namespace Libs::Graphics
