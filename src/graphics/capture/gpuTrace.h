#ifndef KYTY_GRAPHICS_CAPTURE_GPUTRACE_H_
#define KYTY_GRAPHICS_CAPTURE_GPUTRACE_H_

#include "common/common.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::Capture {

constexpr uint16_t TraceMajorVersion = 2;
constexpr uint16_t TraceMinorVersion = 0;

enum class TraceCapability : uint32_t {
	CommandsOnly = 1u << 0u,
};

enum class TraceEventType : uint32_t {
	GraphicsSubmit = 1,
	ComputeSubmit,
	FlipPreparation,
	SuspendRequest,
};

struct TraceEvent {
	uint64_t              sequence = 0;
	TraceEventType        type     = TraceEventType::GraphicsSubmit;
	uint32_t              queue    = 0;
	bool                  trigger_interrupt_on_done = false;
	std::vector<uint32_t> primary;
	std::vector<uint32_t> secondary;
};

struct Trace {
	TraceCapability        capability = TraceCapability::CommandsOnly;
	std::vector<TraceEvent> events;
};

struct TraceReadLimits {
	uint64_t max_events         = 1'000'000;
	uint64_t max_command_dwords = 64ull * 1024ull * 1024ull;
};

[[nodiscard]] bool LoadTrace(const std::filesystem::path& path, Trace& trace, std::string& error,
	                         const TraceReadLimits& limits = {});

class TraceWriter {
public:
	static std::unique_ptr<TraceWriter> Create(const std::filesystem::path& path,
	                                           std::string& error);
	~TraceWriter();
	KYTY_CLASS_NO_COPY(TraceWriter);

	[[nodiscard]] bool RecordGraphics(std::span<const uint32_t> draw,
	                                  std::span<const uint32_t> constants,
	                                  bool trigger_interrupt_on_done, std::string& error);
	[[nodiscard]] bool RecordCompute(uint32_t queue, std::span<const uint32_t> commands,
	                                 bool trigger_interrupt_on_done, std::string& error);
	[[nodiscard]] bool RecordMarker(TraceEventType type, std::string& error);
	[[nodiscard]] bool Finalize(std::string& error);

private:
	explicit TraceWriter(std::filesystem::path path);
	[[nodiscard]] bool Open(std::string& error);
	[[nodiscard]] bool Record(TraceEventType type, uint32_t queue, uint32_t flags,
	                          std::span<const uint32_t> primary,
	                          std::span<const uint32_t> secondary, std::string& error);
	void CloseFile();

	struct Private;
	std::filesystem::path m_path;
	std::unique_ptr<Private> m_private;
};

} // namespace Libs::Graphics::Capture

#endif // KYTY_GRAPHICS_CAPTURE_GPUTRACE_H_
