// Drives the real automation thread (src/libs/automation.cpp) through its command file, with the
// pad API replaced by a recorder. No game, GPU or window is involved.
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "libs/automation.h"
#include "libs/controller.h"

#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <sys/wait.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
namespace Pad = Libs::Controller;
namespace Auto = Libs::Automation;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "AutomationTests:%d: %s\n", __LINE__, #condition);                \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

// ---- recorded pad calls ---------------------------------------------------------------------

struct PadCall {
	enum class Kind { Button, Axis, RightStick, Reset } kind;
	int               id     = 0;
	uint32_t          button = 0;
	bool              down   = false;
	Pad::Axis         axis   = Pad::Axis::LeftX;
	int               a      = 0;
	int               b      = 0;
	Clock::time_point time;
};

std::mutex           g_pad_mutex;
std::vector<PadCall> g_pad_calls;

std::vector<PadCall> PadCalls() {
	std::lock_guard lock(g_pad_mutex);
	return g_pad_calls;
}

void Record(PadCall call) {
	call.time = Clock::now();
	std::lock_guard lock(g_pad_mutex);
	g_pad_calls.push_back(call);
}

} // namespace

namespace Libs::Controller {
void SetButton(int id, uint32_t button, bool down) {
	Record({.kind = PadCall::Kind::Button, .id = id, .button = button, .down = down});
}
void SetAxis(int id, Axis axis, int value) {
	Record({.kind = PadCall::Kind::Axis, .id = id, .axis = axis, .a = value});
}
void SetRightStick(int id, int x, int y) {
	Record({.kind = PadCall::Kind::RightStick, .id = id, .a = x, .b = y});
}
void ResetInputState() {
	Record({.kind = PadCall::Kind::Reset});
}
} // namespace Libs::Controller

namespace Libs::Graphics {
uint64_t MasterSemaphore::QueryGpuTick() const noexcept {
	return 0;
}
} // namespace Libs::Graphics

namespace {

// ---- helpers --------------------------------------------------------------------------------

bool WaitFor(const std::function<bool()>& condition, int timeout_ms = 8000) {
	const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
	while (Clock::now() < deadline) {
		if (condition()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return condition();
}

std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<nlohmann::json> ReadEvents(const std::filesystem::path& dir) {
	std::vector<nlohmann::json> events;
	std::ifstream               in(dir / "events.jsonl");
	for (std::string line; std::getline(in, line);) {
		auto event = nlohmann::json::parse(line, nullptr, false);
		CHECK(event.is_object()); // every line must be a complete JSON object
		events.push_back(std::move(event));
	}
	return events;
}

nlohmann::json ReadStatus(const std::filesystem::path& dir) {
	return nlohmann::json::parse(ReadFile(dir / "status.json"), nullptr, false);
}

void AppendCommands(const std::filesystem::path& dir, const std::string& text) {
	std::ofstream out(dir / "commands.txt", std::ios::app | std::ios::binary);
	out << text;
}

// The ack for command-file line `line`, once the emulator has processed it.
nlohmann::json WaitForAck(const std::filesystem::path& dir, uint64_t line) {
	nlohmann::json found;
	CHECK(WaitFor([&] {
		for (const auto& event: ReadEvents(dir)) {
			if (event.value("event", "") == "ack" && event.value("line", 0ull) == line) {
				found = event;
				return true;
			}
		}
		return false;
	}));
	return found;
}

size_t CountCalls(const std::function<bool(const PadCall&)>& match) {
	size_t count = 0;
	for (const auto& call: PadCalls()) {
		count += match(call) ? 1 : 0;
	}
	return count;
}

void Configure(const std::filesystem::path& automation_dir, const std::filesystem::path& log_file,
               uint32_t shot_interval) {
	Config::ConfigOptions options;
	options.automation_dir           = automation_dir;
	options.automation_shot_interval = shot_interval;
	options.printf_direction         = log_file.empty() ? Config::LogDirection::Silent
	                                                    : Config::LogDirection::File;
	options.printf_output_file       = log_file;
	Config::Load(options);
}

// ---- tests ----------------------------------------------------------------------------------

void TestDisabledIsInert(const std::filesystem::path& root) {
	Configure({}, {}, 0);
	Auto::Start();
	CHECK(!Auto::Enabled());
	Auto::NotePadRead();
	Auto::NoteGuestFlip();
	Auto::NoteGpuSubmit(1, 2, 3, 4, 5, 6, 7, 8);
	Auto::NoteInputReset("ignored");
	CHECK(!Auto::SubmitTraceEnabled() && !Auto::ScreenshotPending());
	CHECK(Auto::TakeScreenshotRequests().empty());
	CHECK(Auto::TakePresentStallSeconds() == 0);
	CHECK(!std::filesystem::exists(root / "disabled"));
}

void TestLogFlush(const std::filesystem::path& root) {
	const auto dir = root / "logflush";
	const auto log = dir / "kyty.txt";
	// With automation on, a line is on disk the moment it is logged: a hung or killed emulator
	// must not lose the tail of its log.
	Configure(root / "auto_log", log, 0);
	Log::Initialize();
	LOGF("flush-check %d\n", 42);
	CHECK(ReadFile(log).find("flush-check 42") != std::string::npos);
	Log::Shutdown();
}

void TestHeartbeatAndCounters(const std::filesystem::path& dir) {
	CHECK(WaitFor([&] { return ReadStatus(dir).is_object(); }));
	const auto first = ReadStatus(dir);
	for (const char* key: {"uptime_ms", "host_presents", "guest_flips", "pad_reads", "schedulers",
	                       "recent_submits", "commands", "shots"}) {
		CHECK(first.contains(key));
	}

	Auto::NotePadRead();
	Auto::NotePadRead();
	Auto::NotePadRead();
	Auto::NoteGuestFlip();
	Auto::NoteGuestFlip();
	Auto::NoteHostPresent();
	Auto::NoteShaderCompile(0x6a53456e7ef5d1b0ull, "cs");
	Auto::NoteGpuSubmit(41, 7, 1001, 1, 2, 3, 4, 0x55);
	Auto::NoteGpuSubmit(42, 9, 1002, 5, 6, 7, 8, 0x66);
	AppendCommands(dir, "status\n");
	CHECK(WaitFor([&] {
		const auto status = ReadStatus(dir);
		return status.is_object() && status.value("pad_reads", 0ull) == 3 &&
		       status.value("submits", 0ull) == 2;
	}));
	const auto status = ReadStatus(dir);
	CHECK(status["guest_flips"] == 2 && status["host_presents"] == 1);
	CHECK(status["last_shader"]["hash"] == "0x6a53456e7ef5d1b0");
	CHECK(status["last_shader"]["stage"] == "cs");
	CHECK(status["recent_submits"].size() == 2);
	// Newest first.
	CHECK(status["recent_submits"][0]["tick"] == 42 && status["recent_submits"][0]["op"] == 9);
	CHECK(status["recent_submits"][1]["submit_id"] == 1001);
	CHECK(status["recent_submits"][0]["args"][4] == 0x66);
}

void TestInput(const std::filesystem::path& dir) {
	const auto first_line = ReadFile(dir / "commands.txt");
	uint64_t   line = static_cast<uint64_t>(std::count(first_line.begin(), first_line.end(), '\n'));

	// Comments and blank lines still count as lines, so a driver can predict ack numbers.
	AppendCommands(dir, "# a comment\n\npress cross 300\n");
	line += 3;
	const auto press_ack = WaitForAck(dir, line);
	CHECK(press_ack["ok"] == true && press_ack["cmd"] == "press cross 300");
	CHECK(WaitFor([] {
		return CountCalls([](const PadCall& c) {
			       return c.kind == PadCall::Kind::Button && !c.down &&
			              c.button == Pad::PAD_BUTTON_CROSS;
		       }) == 1;
	}));
	{
		const auto calls = PadCalls();
		const auto down  = std::ranges::find_if(calls, [](const PadCall& c) {
			return c.kind == PadCall::Kind::Button && c.down && c.button == Pad::PAD_BUTTON_CROSS;
		});
		const auto up = std::ranges::find_if(calls, [](const PadCall& c) {
			return c.kind == PadCall::Kind::Button && !c.down && c.button == Pad::PAD_BUTTON_CROSS;
		});
		CHECK(down != calls.end() && up != calls.end());
		CHECK(down->id == Pad::HOST_INPUT_CONTROLLER_ID);
		const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(up->time - down->time);
		CHECK(held.count() >= 280 && held.count() < 1500);
	}

	// Default press length is longer than one pad poll.
	AppendCommands(dir, "press circle\n");
	line++;
	CHECK(WaitForAck(dir, line)["ok"] == true);
	CHECK(WaitFor([] {
		return CountCalls([](const PadCall& c) {
			       return c.kind == PadCall::Kind::Button && !c.down &&
			              c.button == Pad::PAD_BUTTON_CIRCLE;
		       }) == 1;
	}));
	{
		const auto calls = PadCalls();
		const auto down  = std::ranges::find_if(calls, [](const PadCall& c) {
			return c.kind == PadCall::Kind::Button && c.down && c.button == Pad::PAD_BUTTON_CIRCLE;
		});
		const auto up = std::ranges::find_if(calls, [](const PadCall& c) {
			return c.kind == PadCall::Kind::Button && !c.down && c.button == Pad::PAD_BUTTON_CIRCLE;
		});
		const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(up->time - down->time);
		CHECK(held.count() >= 100);
	}

	// Chords press every button; hold and release are explicit.
	AppendCommands(dir, "press l1+r1 100\nhold square\n");
	line += 2;
	CHECK(WaitForAck(dir, line)["ok"] == true);
	CHECK(CountCalls([](const PadCall& c) {
		      return c.kind == PadCall::Kind::Button && c.down &&
		             (c.button == Pad::PAD_BUTTON_L1 || c.button == Pad::PAD_BUTTON_R1);
	      }) == 2);
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	CHECK(CountCalls([](const PadCall& c) {
		      return c.kind == PadCall::Kind::Button && !c.down && c.button == Pad::PAD_BUTTON_SQUARE;
	      }) == 0); // still held
	AppendCommands(dir, "release square\n");
	line++;
	CHECK(WaitForAck(dir, line)["ok"] == true);
	CHECK(CountCalls([](const PadCall& c) {
		      return c.kind == PadCall::Kind::Button && !c.down && c.button == Pad::PAD_BUTTON_SQUARE;
	      }) == 1);

	// A line is not a command until its newline arrives.
	AppendCommands(dir, "press tri");
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	CHECK(CountCalls([](const PadCall& c) { return c.button == Pad::PAD_BUTTON_TRIANGLE; }) == 0);
	AppendCommands(dir, "angle 100\n");
	line++;
	const auto split_ack = WaitForAck(dir, line);
	CHECK(split_ack["ok"] == true && split_ack["cmd"] == "press triangle 100");

	// Left stick: +y is forward, which is a low pad Y value. It recenters after the duration.
	AppendCommands(dir, "stick l 0 1 300\n");
	line++;
	CHECK(WaitForAck(dir, line)["ok"] == true);
	{
		const auto calls = PadCalls();
		const auto x = std::ranges::find_if(calls, [](const PadCall& c) {
			return c.kind == PadCall::Kind::Axis && c.axis == Pad::Axis::LeftX;
		});
		const auto y = std::ranges::find_if(calls, [](const PadCall& c) {
			return c.kind == PadCall::Kind::Axis && c.axis == Pad::Axis::LeftY;
		});
		CHECK(x != calls.end() && x->a == 128);
		CHECK(y != calls.end() && y->a == 1);
	}
	CHECK(WaitFor([] {
		return CountCalls([](const PadCall& c) {
			       return c.kind == PadCall::Kind::Axis && c.axis == Pad::Axis::LeftY && c.a == 128;
		       }) == 1;
	}));

	AppendCommands(dir, "stick r -1 0\ntrigger r 1 200\n");
	line += 2;
	CHECK(WaitForAck(dir, line)["ok"] == true);
	CHECK(CountCalls([](const PadCall& c) {
		      return c.kind == PadCall::Kind::RightStick && c.a == 1 && c.b == 128;
	      }) == 1);
	CHECK(CountCalls([](const PadCall& c) {
		      return c.kind == PadCall::Kind::Axis && c.axis == Pad::Axis::TriggerRight && c.a == 255;
	      }) == 1);
	CHECK(WaitFor([] {
		return CountCalls([](const PadCall& c) {
			       return c.kind == PadCall::Kind::Axis && c.axis == Pad::Axis::TriggerRight &&
			              c.a == 0;
		       }) == 1;
	}));

	AppendCommands(dir, "reset\n");
	line++;
	CHECK(WaitForAck(dir, line)["ok"] == true);
	CHECK(CountCalls([](const PadCall& c) { return c.kind == PadCall::Kind::Reset; }) == 1);

	// Mistakes are reported, never fatal, and never do anything.
	const auto before = PadCalls().size();
	AppendCommands(dir, "dance\npress nosuchbutton\nstick x 1 1\npress\nstall_present 0\n");
	line += 5;
	CHECK(WaitForAck(dir, line)["ok"] == false);
	for (uint64_t i = line - 4; i <= line; i++) {
		const auto ack = WaitForAck(dir, i);
		CHECK(ack["ok"] == false && !ack["error"].get<std::string>().empty());
	}
	CHECK(WaitForAck(dir, line - 4)["error"].get<std::string>().find("unknown command") != std::string::npos);
	CHECK(WaitForAck(dir, line - 3)["error"].get<std::string>().find("unknown button") != std::string::npos);
	CHECK(PadCalls().size() == before);
}

void TestScreenshots(const std::filesystem::path& dir) {
	// Shots wait for the next presented frame; the presenter polls for them.
	// (The interval timer may add its own "auto" request first, so wait for these two acks.)
	const auto text = ReadFile(dir / "commands.txt");
	const auto line = static_cast<uint64_t>(std::count(text.begin(), text.end(), '\n'));
	AppendCommands(dir, "shot alpha\nshot ../../etc/passwd\n");
	CHECK(WaitForAck(dir, line + 2)["ok"] == true);
	CHECK(Auto::ScreenshotPending());
	const auto names = Auto::TakeScreenshotRequests();
	CHECK(std::ranges::find(names, "alpha") != names.end());
	// Names are sanitized so a command cannot write outside shots/.
	CHECK(std::ranges::find(names, ".._.._etc_passwd") != names.end());

	// 2x1 BGRA: blue then green, alpha 0 (games often leave it zero). PNG must be opaque RGBA.
	const std::vector<uint8_t> bgra = {255, 0, 0, 0, 0, 255, 0, 0};
	Auto::DeliverScreenshot({"alpha", "beta"}, 2, 1, Auto::ShotFormat::Bgra8, bgra);
	const auto alpha = dir / "shots" / "alpha.png";
	CHECK(WaitFor([&] { return std::filesystem::exists(alpha) && std::filesystem::exists(dir / "shots" / "beta.png"); }));
	CHECK(std::filesystem::exists(dir / "latest.png"));
	CHECK(!std::filesystem::exists(dir / ".." / "etc"));
	{
		const auto png = ReadFile(alpha);
		int width = 0, height = 0, channels = 0;
		auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()),
		                                     static_cast<int>(png.size()), &width, &height,
		                                     &channels, 4);
		CHECK(pixels != nullptr && width == 2 && height == 1);
		CHECK(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 255 && pixels[3] == 255); // blue
		CHECK(pixels[4] == 0 && pixels[5] == 255 && pixels[6] == 0 && pixels[7] == 255); // green
		stbi_image_free(pixels);
	}
	CHECK(WaitFor([&] {
		for (const auto& event: ReadEvents(dir)) {
			if (event.value("event", "") == "shot" && event.value("name", "") == "beta" &&
			    event.value("ok", false) && event.value("width", 0) == 2) {
				return true;
			}
		}
		return false;
	}));

	// 10-bit formats are reduced to 8 bits per channel, in the right channel order.
	const uint32_t       red10 = 0x3ffu;                       // A2B10G10R10: R = low 10 bits
	std::vector<uint8_t> packed(4);
	std::memcpy(packed.data(), &red10, 4);
	Auto::DeliverScreenshot({"tenbit"}, 1, 1, Auto::ShotFormat::A2B10G10R10, packed);
	CHECK(WaitFor([&] { return std::filesystem::exists(dir / "shots" / "tenbit.png"); }));
	{
		const auto png = ReadFile(dir / "shots" / "tenbit.png");
		int width = 0, height = 0, channels = 0;
		auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()),
		                                     static_cast<int>(png.size()), &width, &height,
		                                     &channels, 4);
		CHECK(pixels != nullptr && pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0);
		stbi_image_free(pixels);
	}

	Auto::ScreenshotFailed({"broken"}, "unsupported frame format");
	CHECK(WaitFor([&] {
		for (const auto& event: ReadEvents(dir)) {
			if (event.value("event", "") == "shot" && event.value("name", "") == "broken") {
				return !event.value("ok", true) &&
				       event.value("error", "").find("unsupported") != std::string::npos;
			}
		}
		return false;
	}));
}

void TestAutoShotsAndMisc(const std::filesystem::path& dir) {
	// --automation-shot-interval 1: the automation thread keeps asking for a frame, and the
	// result only refreshes latest.png.
	CHECK(WaitFor([] { return Auto::ScreenshotPending(); }, 4000));
	const auto names = Auto::TakeScreenshotRequests();
	CHECK(std::ranges::find(names, "auto") != names.end());
	std::filesystem::remove(dir / "latest.png");
	const std::vector<uint8_t> rgba = {1, 2, 3, 4};
	Auto::DeliverScreenshot({"auto"}, 1, 1, Auto::ShotFormat::Rgba8, rgba);
	CHECK(WaitFor([&] { return std::filesystem::exists(dir / "latest.png"); }));
	CHECK(!std::filesystem::exists(dir / "shots" / "auto.png"));

	Auto::NoteInputReset("overlay took the pad");
	CHECK(WaitFor([&] {
		for (const auto& event: ReadEvents(dir)) {
			if (event.value("event", "") == "input_reset" &&
			    event.value("reason", "") == "overlay took the pad") {
				return true;
			}
		}
		return false;
	}));

	CHECK(!Auto::SubmitTraceEnabled());
	AppendCommands(dir, "trace on\n");
	CHECK(WaitFor([] { return Auto::SubmitTraceEnabled(); }));
	AppendCommands(dir, "trace off\n");
	CHECK(WaitFor([] { return !Auto::SubmitTraceEnabled(); }));

	AppendCommands(dir, "stall_present 2\n");
	CHECK(WaitFor([] { return Auto::TakePresentStallSeconds() == 2; }));
	CHECK(Auto::TakePresentStallSeconds() == 0); // consumed once

	// Event sequence numbers only ever grow.
	uint64_t last = 0;
	for (const auto& event: ReadEvents(dir)) {
		CHECK(event.value("seq", 0ull) > last);
		last = event.value("seq", 0ull);
	}
	CHECK(ReadEvents(dir).front().value("event", "") == "started");
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// `quit` flushes and exits immediately with status 0, from the automation thread.
void TestQuit(const std::filesystem::path& root, const std::filesystem::path& self) {
	const auto dir = root / "quit";
	std::filesystem::create_directories(dir);
	const auto command = "\"" + self.string() + "\" --quit-mode \"" + dir.string() + "\"";
	const int  status  = std::system(command.c_str());
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	bool saw_quit = false;
	for (const auto& event: ReadEvents(dir)) {
		saw_quit |= event.value("event", "") == "quit";
	}
	CHECK(saw_quit);
}
#endif

int RunQuitMode(const std::filesystem::path& dir) {
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Configure(dir, {}, 0);
	Auto::Start();
	AppendCommands(dir, "quit\n");
	std::this_thread::sleep_for(std::chrono::seconds(10));
	return 3; // quit must have ended the process before this
}

} // namespace

int main(int argc, char* argv[]) {
	if (argc > 2 && std::string(argv[1]) == "--quit-mode") {
		return RunQuitMode(argv[2]);
	}

	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();

	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty_automation_tests_" +
	                   std::to_string(Clock::now().time_since_epoch().count()));
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);

	TestDisabledIsInert(root);
	TestLogFlush(root);

	const auto dir = root / "auto";
	Configure(dir, {}, 1);
	Auto::Start();
	CHECK(Auto::Enabled());
	Auto::Start(); // idempotent
	CHECK(WaitFor([&] { return std::filesystem::exists(dir / "events.jsonl"); }));

	TestHeartbeatAndCounters(dir);
	TestInput(dir);
	TestScreenshots(dir);
	TestAutoShotsAndMisc(dir);
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	TestQuit(root, std::filesystem::absolute(argv[0]));
#endif

	std::filesystem::remove_all(root);
	std::puts("Automation tests passed");
	// The automation thread is detached and never stops; leave without running static destructors.
	std::fflush(nullptr);
	std::_Exit(0);
}
