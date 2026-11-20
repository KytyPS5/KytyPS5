#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "libs/guestPrintf.h"
#include "libs/vaContext.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::printf("GuestPrintfTests: %s\n", message);
		std::fflush(stdout);
		std::abort();
	}
}

#ifdef _WIN32
constexpr auto FileNumber = _fileno;
constexpr auto Duplicate = _dup;
constexpr auto Redirect = _dup2;
constexpr auto Close = _close;
#else
constexpr auto FileNumber = fileno;
constexpr auto Duplicate = dup;
constexpr auto Redirect = dup2;
constexpr auto Close = close;
#endif

class StderrCapture {
public:
	StderrCapture() {
		std::fflush(stderr);
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		m_directory = std::filesystem::temp_directory_path() /
		              ("kyty_guest_printf_" + std::to_string(unique));
		Check(std::filesystem::create_directory(m_directory), "could not create capture directory");
		const auto path = m_directory / "stderr.bin";
#ifdef _WIN32
		m_file = _wfopen(path.c_str(), L"w+b");
#else
		m_file = std::fopen(path.c_str(), "w+b");
#endif
		Check(m_file != nullptr, "could not create capture file");
		m_saved = Duplicate(FileNumber(stderr));
		Check(m_saved >= 0 && Redirect(FileNumber(m_file), FileNumber(stderr)) >= 0,
		      "could not redirect stderr");
	}
	~StderrCapture() {
		std::fflush(stderr);
		Redirect(m_saved, FileNumber(stderr));
		Close(m_saved);
		std::fclose(m_file);
		std::error_code error;
		std::filesystem::remove_all(m_directory, error);
	}
	StderrCapture(const StderrCapture&) = delete;
	StderrCapture& operator=(const StderrCapture&) = delete;

	std::string Read() {
		Check(std::fseek(m_file, 0, SEEK_SET) == 0, "could not rewind capture file");
		std::string output;
		std::array<char, 4096> buffer {};
		while (const auto bytes = std::fread(buffer.data(), 1, buffer.size(), m_file)) {
			output.append(buffer.data(), bytes);
		}
		Check(!std::ferror(m_file), "could not read capture file");
		return output;
	}

private:
	FILE* m_file = nullptr;
	int m_saved = -1;
	std::filesystem::path m_directory;
};

void InitializeContext(Libs::VaContext& context) {
	context = {};
	context.va_list.reg_save_area = &context.reg_save_area;
	context.va_list.fp_offset = offsetof(Libs::VaRegSave, fp);
}

void TestPrintedDiagnostics(bool enabled) {
	constexpr char format[] = "Guest fatal: missing file %s (code %d)\n";
	constexpr char path[] = "/app0/content.pak";
	const std::string expected = "Guest fatal: missing file /app0/content.pak (code 2)\n";
	Libs::VaContext context;
	InitializeContext(context);
	context.reg_save_area.gp[0] = reinterpret_cast<uintptr_t>(format);
	context.reg_save_area.gp[1] = reinterpret_cast<uintptr_t>(path);
	context.reg_save_area.gp[2] = 2;
	StderrCapture capture;
	Check(Libs::GetGuestPrintfCtxFunc()(&context) == static_cast<int>(expected.size()),
	      "printf return count changed");
	context.va_list.gp_offset = sizeof(uint64_t);
	Check(Libs::GetGuestVprintfFunc()(format, &context.va_list) == static_cast<int>(expected.size()),
	      "vprintf return count changed");
	const auto output = capture.Read();
	Check(output == (enabled ? "GUEST_STDOUT: " + expected + "GUEST_STDOUT: " + expected : ""),
	      "guest diagnostics were missing, duplicated, unflushed, or enabled without opt-in");
}

void TestLongAndUnterminatedOutput(bool enabled) {
	const std::string text(32769, 'x');
	Libs::VaContext context;
	InitializeContext(context);
	context.reg_save_area.gp[0] = reinterpret_cast<uintptr_t>(text.c_str());
	StderrCapture capture;
	Check(Libs::GetGuestVprintfFunc()("%s", &context.va_list) == static_cast<int>(text.size()),
	      "long printf return count changed");
	Check(capture.Read() == (enabled ? "GUEST_STDOUT: " + text : ""),
	      "forwarding truncated long output or changed its line endings");
}

void TestEmbeddedNullOutput(bool enabled) {
	constexpr char format[] = "%c%c%c";
	for (const auto& text: {std::string("\0ab", 3), std::string("a\0b", 3), std::string("ab\0", 3)}) {
		Libs::VaContext context;
		InitializeContext(context);
		context.reg_save_area.gp[0] = reinterpret_cast<uintptr_t>(format);
		for (size_t i = 0; i < text.size(); i++) {
			context.reg_save_area.gp[i + 1] = static_cast<unsigned char>(text[i]);
		}
		StderrCapture capture;
		Check(Libs::GetGuestPrintfCtxFunc()(&context) == 3,
		      "printf count excluded an embedded null byte");
		context.va_list.gp_offset = sizeof(uint64_t);
		Check(Libs::GetGuestVprintfFunc()(format, &context.va_list) == 3,
		      "vprintf count excluded an embedded null byte");
		const auto expected = std::string("GUEST_STDOUT: ") + text;
		Check(capture.Read() == (enabled ? expected + expected : ""),
		      "forwarding dropped an embedded null byte or the bytes after it");
	}
}

void TestSnprintfDoesNotPrint() {
	std::array<char, 64> buffer {};
	constexpr char format[] = "buffer-only %d";
	Libs::VaContext context;
	InitializeContext(context);
	context.reg_save_area.gp[0] = reinterpret_cast<uintptr_t>(buffer.data());
	context.reg_save_area.gp[1] = buffer.size();
	context.reg_save_area.gp[2] = reinterpret_cast<uintptr_t>(format);
	context.reg_save_area.gp[3] = 42;
	StderrCapture capture;
	Check(Libs::GetGuestSnprintfCtxFunc()(&context) == 14 &&
	          std::strcmp(buffer.data(), "buffer-only 42") == 0,
	      "snprintf formatting or return count changed");
	Check(capture.Read().empty(), "snprintf leaked a buffer-only message to stderr");
}

}

int main(int argc, char** argv) {
	Check(argc == 2, "expected enabled or disabled mode");
	const std::string_view mode(argv[1]);
	Check(mode == "enabled" || mode == "disabled", "invalid test mode");
	Check(std::setvbuf(stderr, nullptr, _IOFBF, 8192) == 0, "could not buffer stderr");
	Config::Initialize();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	Log::Initialize();
	Check(Log::IsSilent(), "test requires the normal logger to be silent");
	TestPrintedDiagnostics(mode == "enabled");
	TestLongAndUnterminatedOutput(mode == "enabled");
	TestEmbeddedNullOutput(mode == "enabled");
	TestSnprintfDoesNotPrint();
	Log::Shutdown();
	Config::Shutdown();
	std::puts("GuestPrintfTests: all cases passed");
}
