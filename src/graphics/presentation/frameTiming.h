#ifndef KYTY_GRAPHICS_PRESENTATION_FRAME_TIMING_H_
#define KYTY_GRAPHICS_PRESENTATION_FRAME_TIMING_H_

#include "common/timer.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace Libs::Graphics {
struct FramePreparationTiming {
	uint64_t wait = 0, lock = 0, resolve = 0, inputs = 0, fg_capture = 0, dlss_record = 0;
};
// Opt-in CPU-side submission timing. This does not measure scanout or GPU time.
// A buffered file keeps diagnostics out of the normal presentation path.
class FrameTimingRecorder {
public:
	FrameTimingRecorder() {
		const char* path = std::getenv("KYTY_FRAME_TIMING_CSV");
		if (path == nullptr || *path == '\0') return;
#ifdef _WIN32
		m_file = _wfopen(std::filesystem::u8path(path).c_str(), L"wb");
#else
		m_file = std::fopen(path, "wb");
#endif
		if (m_file == nullptr) {
			std::fprintf(stderr, "Unable to open KYTY_FRAME_TIMING_CSV: %s\n", path);
			return;
		}
		std::setvbuf(m_file, m_buffer, _IOFBF, sizeof(m_buffer));
		m_frequency = Common::Timer::QueryPerformanceFrequency();
		std::fputs("frame,elapsed_ms,frame_ms,present_ms,new_frame,dlss_evaluated,prepare_wait_ms,prepare_lock_ms,resolve_ms,inputs_ms,fg_capture_ms,dlss_record_ms,display_frames\n", m_file);
	}
	~FrameTimingRecorder() { if (m_file != nullptr) std::fclose(m_file); }
	FrameTimingRecorder(const FrameTimingRecorder&) = delete;
	FrameTimingRecorder& operator=(const FrameTimingRecorder&) = delete;
	[[nodiscard]] bool Enabled() const { return m_file != nullptr; }
	void Record(uint64_t begin, bool new_frame, bool dlss_evaluated,
	            const FramePreparationTiming& preparation = {}, uint32_t display_frames = 0) {
		if (!Enabled()) return;
		const auto now = Common::Timer::QueryPerformanceCounter();
		if (m_start == 0) m_start = now;
		const double to_ms = 1000.0 / static_cast<double>(m_frequency);
		std::fprintf(m_file, "%llu,%.6f,%.6f,%.6f,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u\n",
		    static_cast<unsigned long long>(++m_frames), (now - m_start) * to_ms,
		    m_previous == 0 ? 0.0 : (now - m_previous) * to_ms,
		    (now - begin) * to_ms, new_frame, dlss_evaluated,
		    preparation.wait * to_ms, preparation.lock * to_ms, preparation.resolve * to_ms,
		    preparation.inputs * to_ms, preparation.fg_capture * to_ms, preparation.dlss_record * to_ms,
		    display_frames);
		m_previous = now;
	}

private:
	char m_buffer[65536] {};
	std::FILE* m_file = nullptr;
	uint64_t m_frequency = 0, m_start = 0, m_previous = 0, m_frames = 0;
};
} // namespace Libs::Graphics
#endif
