#include "graphics/shader/shaderReplay.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shaderCapture.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

namespace {

namespace Recompiler = ShaderRecompiler;

struct ReplayOptions {
	bool                  dump     = true;
	bool                  validate = true;
	std::filesystem::path out;
};

const char* DumpLabel(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "ShaderRecompiler VS";
		case ShaderType::Mesh: return "ShaderRecompiler MS";
		case ShaderType::Local: return "ShaderRecompiler LS";
		case ShaderType::TessellationControl: return "ShaderRecompiler HS";
		case ShaderType::TessellationEvaluation: return "ShaderRecompiler DS";
		case ShaderType::Pixel: return "ShaderRecompiler PS";
		case ShaderType::Compute: return "ShaderRecompiler CS";
		default: return "ShaderRecompiler";
	}
}

// Verbose output (compile phases, IR dumps) goes through the console logger. Without it the logger
// is silent, so a fatal error prints once instead of once per sink.
void InitializeEnvironment(bool verbose) {
	static Common::Subsystems subsystems;
	static bool               initialized = false;
	if (initialized) {
		return;
	}
	initialized = true;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = verbose ? Config::LogDirection::Console : Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
}

// Serves the guest memory a capture recorded. Reads the capture never saw are reported instead of
// guessed, because a wrong guess would make a replay disagree with the game.
class ReplayMemory {
public:
	explicit ReplayMemory(const ShaderCaptureData& capture): m_capture(capture) {}

	static bool Read(void* userdata, uint64_t address, std::span<uint32_t> values) {
		return static_cast<ReplayMemory*>(userdata)->Serve(address, values);
	}

	[[nodiscard]] const std::vector<std::pair<uint64_t, size_t>>& Misses() const { return m_misses; }

private:
	bool Serve(uint64_t address, std::span<uint32_t> values) {
		if (values.empty()) {
			return false;
		}
		const uint64_t length = values.size_bytes();
		for (const auto& read: m_capture.reads) {
			const uint64_t read_length = static_cast<uint64_t>(read.values.size()) * sizeof(uint32_t);
			if (address < read.address || address - read.address > read_length ||
			    length > read_length - (address - read.address)) {
				continue;
			}
			if (!read.ok) {
				// The game's read failed at this range too.
				return false;
			}
			std::memcpy(values.data(),
			            reinterpret_cast<const uint8_t*>(read.values.data()) + (address - read.address),
			            length);
			return true;
		}
		// A shader may read constants that sit inside its own code.
		const uint64_t code_length = static_cast<uint64_t>(m_capture.code.size()) * sizeof(uint32_t);
		if (m_capture.shader_base != 0 && address >= m_capture.shader_base &&
		    address - m_capture.shader_base <= code_length &&
		    length <= code_length - (address - m_capture.shader_base)) {
			std::memcpy(values.data(),
			            reinterpret_cast<const uint8_t*>(m_capture.code.data()) +
			                (address - m_capture.shader_base),
			            length);
			return true;
		}
		m_misses.emplace_back(address, values.size());
		return false;
	}

	const ShaderCaptureData&                  m_capture;
	std::vector<std::pair<uint64_t, size_t>> m_misses;
};

bool WriteSpirv(const std::filesystem::path& path, const std::vector<uint32_t>& spirv) {
	if (!path.parent_path().empty()) {
		Common::File::CreateDirectories(path.parent_path());
	}
	Common::File file(path);
	if (file.IsInvalid()) {
		return false;
	}
	uint32_t written = 0;
	file.Write(spirv.data(), static_cast<uint32_t>(spirv.size() * sizeof(uint32_t)), &written);
	return written == spirv.size() * sizeof(uint32_t);
}

int ReplayCapture(const std::filesystem::path& dir, const ReplayOptions& options) {
	ShaderCaptureData capture;
	std::string       error;
	if (!LoadShaderCapture(dir, capture, error)) {
		std::printf("REPLAY ERROR: %s\n", error.c_str());
		return SHADER_REPLAY_EXIT_USAGE;
	}
	std::printf("REPLAY capture=%s stage=%s hash=0x%016" PRIx64
	            " key=%08x code_words=%zu user_data=%zu reads=%zu captured_by=%s\n",
	            Common::PathToString(dir).c_str(), ShaderCaptureStageName(capture.stage), capture.hash,
	            capture.key, capture.code.size(), capture.user_data.size(), capture.reads.size(),
	            capture.git_revision.c_str());

	ShaderVertexInputInfo  vertex {};
	ShaderPixelInputInfo   pixel {};
	ShaderComputeInputInfo compute = capture.compute;
	ShaderStageInputInfo   stage_input {};
	if (capture.raw_input) {
		const bool is_pixel  = capture.stage == ShaderType::Pixel;
		const auto expected  = is_pixel ? sizeof(ShaderPixelInputInfo) : sizeof(ShaderVertexInputInfo);
		if (capture.raw_input_size != expected) {
			std::printf("REPLAY ERROR: input_info.bin is %u bytes but this build expects %zu; replay "
			            "it with the build that captured it (%s)\n",
			            capture.raw_input_size, expected, capture.git_revision.c_str());
			return SHADER_REPLAY_EXIT_USAGE;
		}
		if (capture.git_revision != KYTY_GIT_REVISION) {
			std::printf("REPLAY WARNING: captured by %s, replaying with %s\n",
			            capture.git_revision.c_str(), KYTY_GIT_REVISION);
		}
		if (is_pixel) {
			std::memcpy(&pixel, capture.raw_input_bytes.data(), expected);
			stage_input.pixel = &pixel;
		} else {
			std::memcpy(&vertex, capture.raw_input_bytes.data(), expected);
			stage_input.vertex = &vertex;
		}
	} else {
		stage_input.compute = &compute;
	}

	Recompiler::CompileOptions compile;
	compile.stage          = capture.stage;
	compile.wave_size      = capture.wave_size;
	compile.user_data_base = capture.user_data_base;
	compile.shader_hash    = capture.hash;
	compile.dump_ir        = options.dump;
	compile.early_dump     = options.dump;
	compile.dump_label     = DumpLabel(capture.stage);
	compile.user_data      = capture.user_data;
	compile.back_code      = capture.back_code;
	compile.input_info     = stage_input;

	// Fatal errors raised below end the process with DbgExit, exactly as they do in game.
	auto translated = Recompiler::TranslateProgram(capture.code, compile);
	auto plan       = Recompiler::IR::ExtractResourcePlan(translated.program);

	ReplayMemory                              memory(capture);
	Recompiler::IR::ResourceSnapshot          resources;
	Recompiler::IR::ResourceSpecialization    specialization;
	const Recompiler::IR::SrtRuntime          runtime {
	             .user_data                  = capture.user_data,
	             .shader_base                = capture.shader_base,
	             .userdata                   = &memory,
	             .read_specialization_memory = ReplayMemory::Read,
    };
	if (!Recompiler::IR::MaterializeResources(plan, runtime, resources, specialization)) {
		std::printf("REPLAY FAIL: resource materialization failed (%zu unrecorded guest reads)\n",
		            memory.Misses().size());
		for (const auto& [address, dwords]: memory.Misses()) {
			std::printf("  unrecorded read address=0x%016" PRIx64 " dwords=%zu\n", address, dwords);
		}
		return SHADER_REPLAY_EXIT_MATERIALIZE;
	}
	if (!memory.Misses().empty()) {
		std::printf("REPLAY WARNING: %zu guest reads were not in the capture\n", memory.Misses().size());
		for (const auto& [address, dwords]: memory.Misses()) {
			std::printf("  unrecorded read address=0x%016" PRIx64 " dwords=%zu\n", address, dwords);
		}
	}

	auto result = Recompiler::CompileProgram(std::move(translated), compile, specialization, 0);

	bool valid = true;
	if (options.validate) {
		spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
		std::string          messages;
		tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
		                                     const spv_position_t& position, const char* message) {
			messages += fmt::format("  {}:{} {}\n", static_cast<int>(position.line),
			                        static_cast<int>(position.column), message);
		});
		valid = tools.Validate(result.spirv);
		if (!valid) {
			std::printf("REPLAY FAIL: SPIR-V validation failed\n%s", messages.c_str());
		}
	}

	const auto out = options.out.empty() ? dir / "out.spv" : options.out;
	if (!WriteSpirv(out, result.spirv)) {
		std::printf("REPLAY ERROR: cannot write %s\n", Common::PathToString(out).c_str());
		return SHADER_REPLAY_EXIT_USAGE;
	}
	if (!valid) {
		return SHADER_REPLAY_EXIT_VALIDATE;
	}
	std::printf("REPLAY OK spirv_words=%zu valid=%d out=%s\n", result.spirv.size(),
	            options.validate ? 1 : 0, Common::PathToString(out).c_str());
	return 0;
}

// --- replay-all -----------------------------------------------------------------------------

struct ReplayRow {
	std::string name;
	std::string stage;
	std::string hash;
	std::string key;
	std::string result;
	std::string detail;
	bool        ok = false;
};

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS

std::string ReadTextFile(const std::filesystem::path& path) {
	if (!Common::File::IsFileExisting(path)) {
		return {};
	}
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return {};
	}
	std::string text(static_cast<size_t>(file.Size()), '\0');
	uint32_t    read = 0;
	if (!text.empty()) {
		file.Read(text.data(), static_cast<uint32_t>(text.size()), &read);
	}
	text.resize(read);
	return text;
}

// The first line of text a fatal error printed, or the REPLAY OK/FAIL line.
std::string FirstFatalLine(const std::string& log) {
	size_t position = 0;
	std::string ok_line;
	std::string fail_line;
	while (position < log.size()) {
		auto end = log.find('\n', position);
		if (end == std::string::npos) {
			end = log.size();
		}
		const std::string_view line(log.data() + position, end - position);
		if (line == "--- Error ---" || line == "--- Fatal Error ---") {
			const auto next_begin = std::min(end + 1, log.size());
			auto       next_end   = log.find('\n', next_begin);
			if (next_end == std::string::npos) {
				next_end = log.size();
			}
			return log.substr(next_begin, next_end - next_begin);
		}
		if (line.starts_with("REPLAY OK")) {
			ok_line = std::string(line);
		} else if (line.starts_with("REPLAY FAIL") || line.starts_with("REPLAY ERROR")) {
			fail_line = std::string(line);
		}
		position = end + 1;
	}
	return fail_line.empty() ? ok_line : fail_line;
}

struct RunningChild {
	pid_t                                 pid = -1;
	std::filesystem::path                 dir;
	ReplayRow                             row;
	std::chrono::steady_clock::time_point started;
};

ReplayRow DescribeCapture(const std::filesystem::path& dir) {
	ReplayRow row;
	row.name = Common::PathToString(dir.filename());
	ShaderCaptureData data;
	std::string       error;
	if (LoadShaderCapture(dir, data, error)) {
		row.stage = ShaderCaptureStageName(data.stage);
		row.hash  = fmt::format("0x{:016x}", data.hash);
		row.key   = fmt::format("{:08x}", data.key);
	} else {
		row.stage = "?";
		row.detail = error;
	}
	return row;
}

pid_t SpawnReplay(const std::filesystem::path& dir, const ReplayOptions& options, int timeout) {
	std::fflush(nullptr);
	const pid_t pid = ::fork();
	if (pid != 0) {
		return pid;
	}
	const auto log = Common::PathToString(dir / "replay.log");
	const int  fd  = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		::dup2(fd, STDOUT_FILENO);
		::dup2(fd, STDERR_FILENO);
		::close(fd);
	}
	if (timeout > 0) {
		::alarm(static_cast<unsigned>(timeout));
	}
	const int status = ReplayCapture(dir, options);
	std::fflush(nullptr);
	::_exit(status);
}

void FinishChild(RunningChild& child, int status) {
	const auto detail = FirstFatalLine(ReadTextFile(child.dir / "replay.log"));
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		child.row.ok     = true;
		child.row.result = "OK";
		child.row.detail = detail;
	} else if (WIFEXITED(status)) {
		child.row.result = fmt::format("FAIL({})", WEXITSTATUS(status));
		child.row.detail = detail;
	} else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
		child.row.result = "TIMEOUT";
		child.row.detail = "replay exceeded the time limit";
	} else if (WIFSIGNALED(status)) {
		child.row.result = fmt::format("SIGNAL({})", WTERMSIG(status));
		child.row.detail = detail;
	} else {
		child.row.result = "UNKNOWN";
	}
}

int ReplayAll(const std::filesystem::path& root, const ReplayOptions& options, int jobs,
              int timeout) {
	if (!Common::File::IsDirectoryExisting(root)) {
		std::printf("REPLAY-ALL ERROR: %s is not a directory\n", Common::PathToString(root).c_str());
		return SHADER_REPLAY_EXIT_USAGE;
	}
	std::vector<std::filesystem::path> dirs;
	std::error_code                    ec;
	for (const auto& entry: std::filesystem::directory_iterator(root, ec)) {
		if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "manifest.json", ec)) {
			dirs.push_back(entry.path());
		}
	}
	std::ranges::sort(dirs);
	if (dirs.empty()) {
		std::printf("REPLAY-ALL ERROR: no captures found under %s\n",
		            Common::PathToString(root).c_str());
		return SHADER_REPLAY_EXIT_USAGE;
	}

	std::vector<ReplayRow>    rows;
	std::vector<RunningChild> running;
	size_t                    next = 0;
	while (next < dirs.size() || !running.empty()) {
		while (next < dirs.size() && static_cast<int>(running.size()) < jobs) {
			RunningChild child;
			child.dir     = dirs[next++];
			child.row     = DescribeCapture(child.dir);
			child.started = std::chrono::steady_clock::now();
			child.pid     = SpawnReplay(child.dir, options, timeout);
			if (child.pid < 0) {
				child.row.result = "ERROR";
				child.row.detail = "fork failed";
				rows.push_back(std::move(child.row));
				continue;
			}
			running.push_back(std::move(child));
		}
		if (running.empty()) {
			continue;
		}
		int         status = 0;
		const pid_t done   = ::waitpid(-1, &status, 0);
		if (done < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		const auto found = std::ranges::find(running, done, &RunningChild::pid);
		if (found == running.end()) {
			continue;
		}
		FinishChild(*found, status);
		rows.push_back(std::move(found->row));
		running.erase(found);
	}

	std::ranges::sort(rows, {}, &ReplayRow::name);
	size_t failed = 0;
	std::printf("%-5s %-18s %-8s %-10s %s\n", "STAGE", "HASH", "KEY", "RESULT", "DETAIL");
	nlohmann::json summary = nlohmann::json::array();
	for (const auto& row: rows) {
		if (!row.ok) {
			failed++;
		}
		std::printf("%-5s %-18s %-8s %-10s %s\n", row.stage.c_str(), row.hash.c_str(),
		            row.key.c_str(), row.result.c_str(), row.detail.c_str());
		summary.push_back({{"capture", row.name},
		                   {"stage", row.stage},
		                   {"hash", row.hash},
		                   {"key", row.key},
		                   {"result", row.result},
		                   {"ok", row.ok},
		                   {"detail", row.detail}});
	}
	const auto text = summary.dump(2) + "\n";
	{
		Common::File file(root / "replay_all.json");
		if (!file.IsInvalid()) {
			file.Write(text.data(), static_cast<uint32_t>(text.size()));
		}
	}
	std::printf("REPLAY-ALL %zu captures: %zu ok, %zu failed\n", rows.size(), rows.size() - failed,
	            failed);
	return failed == 0 ? 0 : 1;
}

#else

int ReplayAll(const std::filesystem::path&, const ReplayOptions&, int, int) {
	std::printf("REPLAY-ALL ERROR: not supported on this platform\n");
	return SHADER_REPLAY_EXIT_USAGE;
}

#endif

// --- disassembly ----------------------------------------------------------------------------

int Disassemble(const std::filesystem::path& input, std::optional<uint32_t> pc, uint32_t window) {
	using namespace Recompiler::Decoder;

	auto code_path = input;
	if (Common::File::IsDirectoryExisting(input)) {
		code_path = input / "code.bin";
	}
	if (!Common::File::IsFileExisting(code_path)) {
		std::printf("DISASM ERROR: %s does not exist\n", Common::PathToString(code_path).c_str());
		return SHADER_REPLAY_EXIT_USAGE;
	}
	Common::File file(code_path, Common::File::Mode::Read);
	if (file.IsInvalid() || file.Size() % sizeof(uint32_t) != 0 || file.Size() == 0 ||
	    file.Size() > UINT32_MAX) {
		std::printf("DISASM ERROR: %s is not a whole number of dwords\n",
		            Common::PathToString(code_path).c_str());
		return SHADER_REPLAY_EXIT_USAGE;
	}
	std::vector<uint32_t> code(static_cast<size_t>(file.Size()) / sizeof(uint32_t));
	uint32_t              read = 0;
	file.Read(code.data(), static_cast<uint32_t>(code.size() * sizeof(uint32_t)), &read);
	if (read != code.size() * sizeof(uint32_t)) {
		std::printf("DISASM ERROR: short read\n");
		return SHADER_REPLAY_EXIT_USAGE;
	}

	// Instructions have variable length, so decoding always starts at the first word. A fatal
	// decode error ends the process, so everything printed so far stays visible.
	std::deque<std::string> before;
	uint32_t                after   = 0;
	bool                    reached = !pc.has_value();
	for (size_t word = 0; word < code.size();) {
		Instruction inst;
		DecodeInstruction(code, static_cast<uint32_t>(word), inst);
		const uint32_t size = std::max(inst.word_count, 1u);
		auto           line = InstructionToString(inst);
		if (!pc.has_value()) {
			std::printf("%s\n", line.c_str());
		} else if (!reached) {
			const bool target = pc.value() >= inst.pc && pc.value() < inst.pc + size * 4u;
			if (target) {
				for (const auto& earlier: before) {
					std::printf("   %s\n", earlier.c_str());
				}
				std::printf("=> %s\n", line.c_str());
				reached = true;
			} else {
				before.push_back(std::move(line));
				if (before.size() > window) {
					before.pop_front();
				}
			}
		} else {
			std::printf("   %s\n", line.c_str());
			if (++after >= window) {
				return 0;
			}
		}
		word += size;
	}
	if (!reached) {
		std::printf("DISASM ERROR: pc 0x%x is outside the shader (0x%zx bytes)\n", pc.value(),
		            code.size() * sizeof(uint32_t));
		return SHADER_REPLAY_EXIT_USAGE;
	}
	return 0;
}

// --- command line ---------------------------------------------------------------------------

bool ParseNumber(const char* text, uint64_t& out) {
	if (text == nullptr || *text == '\0') {
		return false;
	}
	char*      end   = nullptr;
	const auto value = std::strtoull(text, &end, 0);
	if (*end != '\0') {
		return false;
	}
	out = value;
	return true;
}

int Usage(const char* message) {
	std::printf("%s\n", message);
	std::printf("usage: kyty_emulator --shader-replay <capture> [--out file.spv] [--no-dump] "
	            "[--no-validate]\n"
	            "       kyty_emulator --shader-replay-all <captures> [--jobs N] [--timeout seconds] "
	            "[--dump]\n"
	            "       kyty_emulator --shader-disasm <code.bin|capture> [--pc 0x86c] [--window 20]\n");
	return SHADER_REPLAY_EXIT_USAGE;
}

} // namespace

bool IsShaderToolCommand(const char* arg) {
	const std::string_view text(arg);
	return text == "--shader-replay" || text == "--shader-replay-all" || text == "--shader-disasm";
}

int RunShaderToolCommand(int argc, char* argv[]) {
	const std::string command = argv[1];
	if (argc < 3) {
		return Usage(("missing argument for " + command).c_str());
	}
	const std::filesystem::path target = Common::PathFromUtf8(argv[2]);

	ReplayOptions options;
	options.dump = command == "--shader-replay";
	int                      jobs    = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
	int                      timeout = 300;
	std::optional<uint32_t>  pc;
	uint32_t                 window = 20;

	for (int i = 3; i < argc; i++) {
		const std::string flag = argv[i];
		uint64_t          number = 0;
		if (flag == "--no-dump") {
			options.dump = false;
		} else if (flag == "--dump") {
			options.dump = true;
		} else if (flag == "--no-validate") {
			options.validate = false;
		} else if (flag == "--out" && i + 1 < argc) {
			options.out = Common::PathFromUtf8(argv[++i]);
		} else if (flag == "--jobs" && i + 1 < argc && ParseNumber(argv[++i], number) && number > 0) {
			jobs = static_cast<int>(std::min<uint64_t>(number, 64));
		} else if (flag == "--timeout" && i + 1 < argc && ParseNumber(argv[++i], number)) {
			timeout = static_cast<int>(std::min<uint64_t>(number, 86400));
		} else if (flag == "--pc" && i + 1 < argc && ParseNumber(argv[++i], number) &&
		           number <= UINT32_MAX) {
			pc = static_cast<uint32_t>(number);
		} else if (flag == "--window" && i + 1 < argc && ParseNumber(argv[++i], number) &&
		           number <= 100000) {
			window = static_cast<uint32_t>(number);
		} else {
			return Usage(("unknown or incomplete option " + flag).c_str());
		}
	}

	InitializeEnvironment(options.dump);
	if (command == "--shader-replay") {
		return ReplayCapture(target, options);
	}
	if (command == "--shader-replay-all") {
		return ReplayAll(target, options, jobs, timeout);
	}
	return Disassemble(target, pc, window);
}

} // namespace Libs::Graphics
