#ifndef KYTY_GRAPHICS_REGRESSION_FRAMEREGRESSION_H_
#define KYTY_GRAPHICS_REGRESSION_FRAMEREGRESSION_H_

#include "common/common.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::Regression {

enum class Mode { Record, Compare };

enum class PixelFormat : uint32_t {
	Rgba8Unorm = 1,
	Rgba8Srgb,
	Bgra8Unorm,
	Bgra8Srgb,
	A2b10g10r10Unorm,
	A2r10g10b10Unorm,
	Rgba16Float,
};

struct Options {
	Mode                  mode = Mode::Compare;
	std::filesystem::path baseline;
	std::filesystem::path report;
	std::vector<uint64_t> frame_ordinals;
	bool                  save_raw_frames = true;
};

struct FrameView {
	uint64_t                 ordinal  = 0;
	uint32_t                 width    = 0;
	uint32_t                 height   = 0;
	uint32_t                 row_pitch = 0;
	PixelFormat              format   = PixelFormat::Rgba8Unorm;
	std::span<const uint8_t> bytes;
};

class Session {
public:
	static std::unique_ptr<Session> Create(Options options, std::string& error);
	~Session();
	KYTY_CLASS_NO_COPY(Session);

	[[nodiscard]] bool WantsFrame(uint64_t ordinal) const;
	[[nodiscard]] bool Observe(const FrameView& frame, std::string& error);
	[[nodiscard]] bool Complete() const;
	[[nodiscard]] int  Finalize(std::string& error);
	[[nodiscard]] int  ExitCode() const;

private:
	explicit Session(Options options);
	[[nodiscard]] bool Initialize(std::string& error);
	[[nodiscard]] bool WriteManifest(bool complete, std::string& error) const;

	struct Private;
	Options                  m_options;
	std::unique_ptr<Private> m_private;
};

[[nodiscard]] uint32_t BytesPerPixel(PixelFormat format);

} // namespace Libs::Graphics::Regression

#endif // KYTY_GRAPHICS_REGRESSION_FRAMEREGRESSION_H_
