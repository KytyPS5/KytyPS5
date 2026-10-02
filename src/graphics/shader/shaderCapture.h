#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCAPTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCAPTURE_H_

#include "common/common.h"
#include "graphics/shader/shader.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Common {
class File;
}

// A shader capture is written before a program is compiled, so every later failure (resource
// tracking, SPIR-V emission, driver crash) leaves a directory that `--shader-replay` can compile
// again without launching the game:
//
//   <dir>/<stage>_<hash>_<key>/
//     code.bin         guest shader code
//     back_code.bin    second code region of a merged GS front half (only when present)
//     user_data.bin    user SGPR values
//     input_info.bin   raw stage input struct (every stage except compute)
//     manifest.json    everything else needed to rebuild the compile inputs
//     reads.bin        guest memory reads made while the program was prepared
namespace Libs::Graphics {

inline constexpr uint32_t SHADER_CAPTURE_FORMAT = 1;

[[nodiscard]] const char* ShaderCaptureStageName(ShaderType stage);
[[nodiscard]] bool        ShaderCaptureStageFromName(const std::string& name, ShaderType& stage);

struct ShaderCaptureSource {
	ShaderType                stage          = ShaderType::Unknown;
	uint64_t                  hash           = 0;
	uint32_t                  wave_size      = 64;
	uint32_t                  user_data_base = 0;
	uint64_t                  shader_base    = 0;
	std::span<const uint32_t> code;
	std::span<const uint32_t> back_code;
	std::span<const uint32_t> user_data;
	std::span<const uint32_t> static_state;
};

struct ShaderCaptureRead {
	uint64_t              address = 0;
	bool                  ok      = false;
	std::vector<uint32_t> values;
};

// Everything `LoadShaderCapture` recovers from a capture directory.
struct ShaderCaptureData {
	ShaderType            stage = ShaderType::Unknown;
	uint64_t              hash  = 0;
	uint32_t              key   = 0;
	uint32_t              wave_size      = 64;
	uint32_t              user_data_base = 0;
	uint64_t              shader_base    = 0;
	std::vector<uint32_t> code;
	std::vector<uint32_t> back_code;
	std::vector<uint32_t> user_data;
	std::vector<uint32_t> static_state;
	// Compute stages record named fields. Other stages carry a raw dump of the input struct, which
	// is only meaningful to the build that wrote it.
	bool                   raw_input = false;
	uint32_t               raw_input_size = 0;
	std::vector<uint8_t>   raw_input_bytes;
	ShaderComputeInputInfo compute;
	std::string            git_revision;
	std::vector<ShaderCaptureRead> reads;
};

// Writes a capture directory and records guest reads until it is destroyed. A disabled or failed
// capture is silent: capturing never changes what the emulator does.
class ShaderCapture {
public:
	ShaderCapture(const ShaderCaptureSource& source, const ShaderComputeInputInfo& input);
	ShaderCapture(const ShaderCaptureSource& source, const ShaderPixelInputInfo& input);
	ShaderCapture(const ShaderCaptureSource& source, const ShaderVertexInputInfo& input);
	~ShaderCapture();
	KYTY_CLASS_NO_COPY(ShaderCapture);

	[[nodiscard]] const std::filesystem::path& Directory() const noexcept { return m_dir; }

	// Called by the guest-memory reader. Only reads made by the thread that owns the capture count.
	static void RecordRead(uint64_t address, std::span<const uint32_t> values, bool ok);

private:
	void Begin(const ShaderCaptureSource& source, const void* raw_input, size_t raw_input_size,
	           const ShaderComputeInputInfo* compute);
	void Append(uint64_t address, std::span<const uint32_t> values, bool ok);

	std::filesystem::path        m_dir;
	std::unique_ptr<Common::File> m_reads;
	ShaderCapture*               m_previous = nullptr;
};

// 32-bit digest of the program key (stage, hash, user data count, code size, static state).
[[nodiscard]] uint32_t ShaderCaptureKey(ShaderType stage, uint64_t hash, uint32_t user_data_count,
                                        uint32_t code_size_words,
                                        std::span<const uint32_t> static_state);

[[nodiscard]] bool LoadShaderCapture(const std::filesystem::path& dir, ShaderCaptureData& out,
                                     std::string& error);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCAPTURE_H_ */
