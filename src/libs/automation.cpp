#include "libs/automation.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "kytyGitVersion.h"
#include "libs/controller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <sys/prctl.h>
#include <unistd.h>
#endif

// stb_image_write is private to this file, like it is in libPngEnc.cpp.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace Libs::Automation {

namespace {

using Clock = std::chrono::steady_clock;
namespace Pad = Libs::Controller;

constexpr auto POLL_INTERVAL      = std::chrono::milliseconds(16);
constexpr auto HEARTBEAT_INTERVAL = std::chrono::milliseconds(500);
constexpr int  DEFAULT_PRESS_MS   = 120; // Longer than one pad poll, so the game always sees it.
constexpr int  MAX_HOLD_MS        = 10 * 60 * 1000;
constexpr size_t RECENT_SUBMITS   = 64;
constexpr size_t REPORTED_SUBMITS = 8;

struct ButtonName {
	const char* name;
	uint32_t    mask;
};

constexpr std::array<ButtonName, 16> BUTTONS {{
    {"cross", Pad::PAD_BUTTON_CROSS},
    {"circle", Pad::PAD_BUTTON_CIRCLE},
    {"square", Pad::PAD_BUTTON_SQUARE},
    {"triangle", Pad::PAD_BUTTON_TRIANGLE},
    {"l1", Pad::PAD_BUTTON_L1},
    {"r1", Pad::PAD_BUTTON_R1},
    {"l2", Pad::PAD_BUTTON_L2},
    {"r2", Pad::PAD_BUTTON_R2},
    {"l3", Pad::PAD_BUTTON_L3},
    {"r3", Pad::PAD_BUTTON_R3},
    {"options", Pad::PAD_BUTTON_OPTIONS},
    {"touchpad", Pad::PAD_BUTTON_TOUCH_PAD},
    {"up", Pad::PAD_BUTTON_UP},
    {"down", Pad::PAD_BUTTON_DOWN},
    {"left", Pad::PAD_BUTTON_LEFT},
    {"right", Pad::PAD_BUTTON_RIGHT},
}};

struct SubmitRecord {
	std::atomic<uint64_t> tick {0};
	std::atomic<uint64_t> submit_id {0};
	std::atomic<uint64_t> arg4 {0};
	std::atomic<uint32_t> op {0};
	std::atomic<uint32_t> arg0 {0};
	std::atomic<uint32_t> arg1 {0};
	std::atomic<uint32_t> arg2 {0};
	std::atomic<uint32_t> arg3 {0};
};

struct Result {
	bool        ok = true;
	std::string error;
};

Result Fail(std::string message) {
	return {false, std::move(message)};
}

// Everything the automation thread and the hooks share. Allocated once and never freed: the
// thread outlives every subsystem that could tear it down.
struct State {
	std::filesystem::path dir;
	std::filesystem::path shots_dir;
	std::filesystem::path commands_path;
	std::filesystem::path events_path;
	std::filesystem::path status_path;
	std::filesystem::path latest_path;
	Clock::time_point     start = Clock::now();
	uint32_t              shot_interval_seconds = 0;

	// Hooks write these from any thread.
	std::atomic<uint64_t> guest_flips {0};
	std::atomic<uint64_t> host_presents {0};
	std::atomic<uint64_t> pad_reads {0};
	std::atomic<uint64_t> input_resets {0};
	std::atomic<uint64_t> shots_taken {0};
	std::atomic<uint64_t> commands_run {0};
	std::atomic<uint64_t> shader_hash {0};
	std::atomic<const char*> shader_stage {nullptr};
	std::atomic<int64_t>  shader_time_ms {0};
	std::atomic<uint64_t> submit_cursor {0};
	std::array<SubmitRecord, RECENT_SUBMITS> submits;
	std::atomic<bool>     trace_submits {false};
	std::atomic<uint32_t> stall_seconds {0};

	std::mutex                    queue_mutex;
	std::deque<nlohmann::json>    events;
	std::vector<std::string>      shot_requests;
	std::atomic<bool>             shot_pending {false};
	struct Delivery {
		std::vector<std::string> names;
		uint32_t                 width  = 0;
		uint32_t                 height = 0;
		ShotFormat               format = ShotFormat::Rgba8;
		std::vector<uint8_t>     pixels;
		std::string              error;
	};
	std::deque<Delivery> deliveries;

	// Only the automation thread touches the rest.
	uint64_t                         command_offset = 0;
	uint64_t                         command_line   = 0;
	std::string                      command_buffer;
	std::map<uint32_t, Clock::time_point> release_buttons;
	std::array<std::optional<Clock::time_point>, 2> release_sticks;
	std::array<std::optional<Clock::time_point>, 2> release_triggers;
	Clock::time_point                next_heartbeat = Clock::now();
	Clock::time_point                next_auto_shot = Clock::now();
	Clock::time_point                last_heartbeat = Clock::now();
	uint64_t                         last_flips     = 0;
	uint64_t                         last_presents  = 0;
	uint64_t                         auto_shots     = 0;
	uint64_t                         event_seq      = 0;
};

std::atomic<bool> g_enabled {false};
State*            g_state = nullptr;

// Command schedulers are created long before Start(), so they register here rather than in State.
std::mutex                              g_ticks_mutex;
std::vector<Graphics::MasterSemaphore*> g_schedulers;

int64_t UptimeMs(const State& s) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.start).count();
}

std::string Hex64(uint64_t value) {
	return fmt::format("0x{:016x}", value);
}

void PostEvent(State& s, nlohmann::json event) {
	std::lock_guard lock(s.queue_mutex);
	s.events.push_back(std::move(event));
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
	Common::File file(path);
	if (!file.IsInvalid()) {
		file.Write(text.data(), static_cast<uint32_t>(text.size()));
	}
}

void DrainEvents(State& s) {
	std::deque<nlohmann::json> events;
	{
		std::lock_guard lock(s.queue_mutex);
		events.swap(s.events);
	}
	if (events.empty()) {
		return;
	}
	std::ofstream out(s.events_path, std::ios::app | std::ios::binary);
	for (auto& event: events) {
		event["t"]   = UptimeMs(s);
		event["seq"] = ++s.event_seq;
		out << event.dump() << '\n';
	}
	out.flush();
}

// ---- input --------------------------------------------------------------------------------

bool ParseButtons(const std::string& text, uint32_t& mask, std::string& error) {
	mask = 0;
	std::stringstream parts(text);
	std::string       name;
	while (std::getline(parts, name, '+')) {
		const auto found = std::ranges::find_if(
		    BUTTONS, [&](const ButtonName& button) { return name == button.name; });
		if (found == BUTTONS.end()) {
			error = fmt::format("unknown button '{}'", name);
			return false;
		}
		mask |= found->mask;
	}
	if (mask == 0) {
		error = "no buttons given";
		return false;
	}
	return true;
}

void SetButtons(uint32_t mask, bool down) {
	for (const auto& button: BUTTONS) {
		if ((mask & button.mask) != 0) {
			Pad::SetButton(Pad::HOST_INPUT_CONTROLLER_ID, button.mask, down);
		}
	}
}

int AxisValue(double value, bool invert) {
	const double clamped = std::clamp(invert ? -value : value, -1.0, 1.0);
	return std::clamp(static_cast<int>(std::lround(128.0 + clamped * 127.0)), 0, 255);
}

void SetStick(int stick, double x, double y) {
	const int ax = AxisValue(x, false);
	const int ay = AxisValue(y, true); // Pad Y grows downward; +1 here means forward.
	if (stick == 0) {
		Pad::SetAxis(Pad::HOST_INPUT_CONTROLLER_ID, Pad::Axis::LeftX, ax);
		Pad::SetAxis(Pad::HOST_INPUT_CONTROLLER_ID, Pad::Axis::LeftY, ay);
	} else {
		Pad::SetRightStick(Pad::HOST_INPUT_CONTROLLER_ID, ax, ay);
	}
}

void SetTrigger(int trigger, int value) {
	Pad::SetAxis(Pad::HOST_INPUT_CONTROLLER_ID,
	             trigger == 0 ? Pad::Axis::TriggerLeft : Pad::Axis::TriggerRight, value);
}

bool ParseNumber(const std::string& text, double& out) {
	char*      end   = nullptr;
	const auto value = std::strtod(text.c_str(), &end);
	if (text.empty() || *end != '\0' || !std::isfinite(value)) {
		return false;
	}
	out = value;
	return true;
}

bool ParseMs(const std::string& text, int& out) {
	double value = 0;
	if (!ParseNumber(text, value) || value < 0 || value > MAX_HOLD_MS) {
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

void ReleaseAll(State& s) {
	s.release_buttons.clear();
	s.release_sticks   = {};
	s.release_triggers = {};
	Pad::ResetInputState();
}

std::string SanitizeName(const std::string& name) {
	std::string clean;
	for (const char c: name) {
		clean += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.')
		             ? c
		             : '_';
	}
	return clean;
}

Result RunCommand(State& s, const std::string& line) {
	std::istringstream       in(line);
	std::vector<std::string> words;
	for (std::string word; in >> word;) {
		words.push_back(word);
	}
	const auto&  command = words[0];
	const auto   now     = Clock::now();
	std::string  error;

	if (command == "press" || command == "hold" || command == "release") {
		if (words.size() < 2) {
			return Fail(command + " needs a button");
		}
		if (command == "release" && words[1] == "all") {
			ReleaseAll(s);
			return {};
		}
		uint32_t mask = 0;
		if (!ParseButtons(words[1], mask, error)) {
			return Fail(error);
		}
		if (command == "release") {
			for (auto it = s.release_buttons.begin(); it != s.release_buttons.end();) {
				it = (it->first & mask) != 0 ? s.release_buttons.erase(it) : std::next(it);
			}
			SetButtons(mask, false);
			return {};
		}
		int ms = DEFAULT_PRESS_MS;
		if (command == "press" && words.size() > 2 && !ParseMs(words[2], ms)) {
			return Fail("bad duration '" + words[2] + "'");
		}
		SetButtons(mask, true);
		for (const auto& button: BUTTONS) {
			if ((mask & button.mask) == 0) {
				continue;
			}
			if (command == "hold") {
				s.release_buttons.erase(button.mask);
			} else {
				auto& due = s.release_buttons[button.mask];
				due       = std::max(due, now + std::chrono::milliseconds(std::max(ms, 1)));
			}
		}
		return {};
	}

	if (command == "stick") {
		if (words.size() < 4 || (words[1] != "l" && words[1] != "r")) {
			return Fail("usage: stick <l|r> <x> <y> [ms]");
		}
		double x = 0;
		double y = 0;
		int    ms = 0;
		if (!ParseNumber(words[2], x) || !ParseNumber(words[3], y) ||
		    (words.size() > 4 && !ParseMs(words[4], ms))) {
			return Fail("bad stick arguments");
		}
		const int stick = words[1] == "l" ? 0 : 1;
		SetStick(stick, x, y);
		s.release_sticks[stick] = ms > 0 ? std::optional(now + std::chrono::milliseconds(ms))
		                                 : std::nullopt;
		return {};
	}

	if (command == "trigger") {
		double value = 0;
		int    ms    = 0;
		if (words.size() < 3 || (words[1] != "l" && words[1] != "r") ||
		    !ParseNumber(words[2], value) || (words.size() > 3 && !ParseMs(words[3], ms))) {
			return Fail("usage: trigger <l|r> <0..1> [ms]");
		}
		const int trigger = words[1] == "l" ? 0 : 1;
		SetTrigger(trigger, std::clamp(static_cast<int>(std::lround(value * 255.0)), 0, 255));
		s.release_triggers[trigger] =
		    ms > 0 ? std::optional(now + std::chrono::milliseconds(ms)) : std::nullopt;
		return {};
	}

	if (command == "reset") {
		ReleaseAll(s);
		return {};
	}

	if (command == "shot") {
		const auto name = words.size() > 1 ? SanitizeName(words[1])
		                                   : fmt::format("shot_{}", s.shots_taken.load() + 1);
		{
			std::lock_guard lock(s.queue_mutex);
			s.shot_requests.push_back(name);
		}
		s.shot_pending.store(true, std::memory_order_release);
		return {};
	}

	if (command == "status") {
		s.next_heartbeat = now;
		return {};
	}

	if (command == "trace") {
		if (words.size() < 2 || (words[1] != "on" && words[1] != "off")) {
			return Fail("usage: trace <on|off>");
		}
		s.trace_submits.store(words[1] == "on");
		return {};
	}

	if (command == "stall_present") {
		double seconds = 0;
		if (words.size() < 2 || !ParseNumber(words[1], seconds) || seconds <= 0 || seconds > 3600) {
			return Fail("usage: stall_present <seconds>");
		}
		s.stall_seconds.store(static_cast<uint32_t>(seconds));
		return {};
	}

	if (command == "quit") {
		PostEvent(s, {{"event", "quit"}});
		DrainEvents(s);
		Log::Flush();
		Common::DbgExit(0);
	}

	return Fail("unknown command '" + command + "'");
}

void PollCommands(State& s) {
	std::error_code ec;
	const auto      size = std::filesystem::file_size(s.commands_path, ec);
	if (ec) {
		return;
	}
	if (size < s.command_offset) {
		// Someone truncated the file; start over.
		s.command_offset = 0;
		s.command_line   = 0;
		s.command_buffer.clear();
	}
	if (size == s.command_offset) {
		return;
	}
	std::ifstream in(s.commands_path, std::ios::binary);
	in.seekg(static_cast<std::streamoff>(s.command_offset));
	std::string chunk(static_cast<size_t>(size - s.command_offset), '\0');
	in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
	chunk.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0)));
	s.command_offset += chunk.size();
	s.command_buffer += chunk;

	for (size_t newline = s.command_buffer.find('\n'); newline != std::string::npos;
	     newline        = s.command_buffer.find('\n')) {
		std::string line = s.command_buffer.substr(0, newline);
		s.command_buffer.erase(0, newline + 1);
		s.command_line++;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#') {
			continue;
		}
		line = line.substr(first);
		s.commands_run++;
		const auto result = RunCommand(s, line);
		PostEvent(s, {{"event", "ack"},
		              {"line", s.command_line},
		              {"cmd", line},
		              {"ok", result.ok},
		              {"error", result.error}});
	}
}

void FireTimedActions(State& s, Clock::time_point now) {
	for (auto it = s.release_buttons.begin(); it != s.release_buttons.end();) {
		if (it->second <= now) {
			Pad::SetButton(Pad::HOST_INPUT_CONTROLLER_ID, it->first, false);
			it = s.release_buttons.erase(it);
		} else {
			++it;
		}
	}
	for (int stick = 0; stick < 2; stick++) {
		if (s.release_sticks[stick] && *s.release_sticks[stick] <= now) {
			SetStick(stick, 0.0, 0.0);
			s.release_sticks[stick].reset();
		}
		if (s.release_triggers[stick] && *s.release_triggers[stick] <= now) {
			SetTrigger(stick, 0);
			s.release_triggers[stick].reset();
		}
	}
}

// ---- screenshots --------------------------------------------------------------------------

struct PngBuffer {
	std::vector<uint8_t> bytes;
};

void AppendPng(void* context, void* data, int size) {
	auto& buffer = *static_cast<PngBuffer*>(context);
	const auto* begin = static_cast<const uint8_t*>(data);
	buffer.bytes.insert(buffer.bytes.end(), begin, begin + size);
}

// Converts a presented frame to opaque RGBA8.
std::vector<uint8_t> ToRgba8(const State::Delivery& shot) {
	const size_t         pixels = static_cast<size_t>(shot.width) * shot.height;
	std::vector<uint8_t> rgba(pixels * 4);
	const auto*          src = shot.pixels.data();
	for (size_t i = 0; i < pixels; i++) {
		uint8_t* dst = &rgba[i * 4];
		switch (shot.format) {
			case ShotFormat::Rgba8:
				dst[0] = src[i * 4];
				dst[1] = src[i * 4 + 1];
				dst[2] = src[i * 4 + 2];
				break;
			case ShotFormat::Bgra8:
				dst[0] = src[i * 4 + 2];
				dst[1] = src[i * 4 + 1];
				dst[2] = src[i * 4];
				break;
			case ShotFormat::A2B10G10R10:
			case ShotFormat::A2R10G10B10: {
				uint32_t packed = 0;
				std::memcpy(&packed, src + i * 4, sizeof(packed));
				const auto low  = static_cast<uint8_t>(((packed >> 0u) & 0x3ffu) >> 2u);
				const auto mid  = static_cast<uint8_t>(((packed >> 10u) & 0x3ffu) >> 2u);
				const auto high = static_cast<uint8_t>(((packed >> 20u) & 0x3ffu) >> 2u);
				const bool abgr = shot.format == ShotFormat::A2B10G10R10;
				dst[0]          = abgr ? low : high;
				dst[1]          = mid;
				dst[2]          = abgr ? high : low;
				break;
			}
		}
		dst[3] = 0xff;
	}
	return rgba;
}

void SaveDelivery(State& s, State::Delivery& shot) {
	if (!shot.error.empty()) {
		for (const auto& name: shot.names) {
			PostEvent(s, {{"event", "shot"}, {"name", name}, {"ok", false}, {"error", shot.error}});
		}
		return;
	}
	stbi_write_png_compression_level = 4;
	const auto rgba                  = ToRgba8(shot);
	PngBuffer  png;
	if (stbi_write_png_to_func(AppendPng, &png, static_cast<int>(shot.width),
	                           static_cast<int>(shot.height), 4, rgba.data(),
	                           static_cast<int>(shot.width) * 4) == 0 ||
	    png.bytes.empty()) {
		for (const auto& name: shot.names) {
			PostEvent(s, {{"event", "shot"}, {"name", name}, {"ok", false}, {"error", "png encode failed"}});
		}
		return;
	}
	const auto write = [&](const std::filesystem::path& path) {
		Common::File file(path);
		if (file.IsInvalid()) {
			return false;
		}
		uint32_t written = 0;
		file.Write(png.bytes.data(), static_cast<uint32_t>(png.bytes.size()), &written);
		return written == png.bytes.size();
	};
	// Named shots live in shots/. The file is renamed into place so a reader never sees half a PNG.
	for (const auto& name : shot.names) {
		if (name == "auto") {
			continue;
		}
		const auto path = s.shots_dir / (name + ".png");
		const auto tmp  = std::filesystem::path(path.string() + ".tmp");
		const bool ok   = write(tmp);
		std::error_code ec;
		if (ok) {
			std::filesystem::rename(tmp, path, ec);
		}
		s.shots_taken++;
		PostEvent(s, {{"event", "shot"},
		              {"name", name},
		              {"ok", ok && !ec},
		              {"path", Common::PathToString(path)},
		              {"width", shot.width},
		              {"height", shot.height}});
	}
	const auto tmp = std::filesystem::path(s.latest_path.string() + ".tmp");
	if (write(tmp)) {
		std::error_code ec;
		std::filesystem::rename(tmp, s.latest_path, ec);
	}
	if (std::ranges::find(shot.names, "auto") != shot.names.end()) {
		s.shots_taken++;
		PostEvent(s, {{"event", "shot"},
		              {"name", "auto"},
		              {"ok", true},
		              {"path", Common::PathToString(s.latest_path)},
		              {"width", shot.width},
		              {"height", shot.height}});
	}
}

void ProcessDeliveries(State& s) {
	for (;;) {
		State::Delivery shot;
		{
			std::lock_guard lock(s.queue_mutex);
			if (s.deliveries.empty()) {
				return;
			}
			shot = std::move(s.deliveries.front());
			s.deliveries.pop_front();
		}
		SaveDelivery(s, shot);
	}
}

void MaybeAutoShot(State& s, Clock::time_point now) {
	if (s.shot_interval_seconds == 0 || now < s.next_auto_shot) {
		return;
	}
	s.next_auto_shot = now + std::chrono::seconds(s.shot_interval_seconds);
	{
		std::lock_guard lock(s.queue_mutex);
		s.shot_requests.push_back("auto");
	}
	s.shot_pending.store(true, std::memory_order_release);
}

// ---- heartbeat ----------------------------------------------------------------------------

void WriteStatus(State& s, Clock::time_point now) {
	const double elapsed = std::chrono::duration<double>(now - s.last_heartbeat).count();
	const auto   flips    = s.guest_flips.load();
	const auto   presents = s.host_presents.load();
	double       guest_fps = 0;
	double       host_fps  = 0;
	if (elapsed > 0.05) {
		guest_fps = static_cast<double>(flips - s.last_flips) / elapsed;
		host_fps  = static_cast<double>(presents - s.last_presents) / elapsed;
		s.last_heartbeat = now;
		s.last_flips     = flips;
		s.last_presents  = presents;
	}

	nlohmann::json status;
	status["uptime_ms"]      = UptimeMs(s);
#if defined(__linux__)
	status["pid"] = static_cast<int64_t>(::getpid());
#endif
	status["build"]          = KYTY_BUILD_LABEL;
	status["host_presents"]  = presents;
	status["guest_flips"]    = flips;
	status["host_fps"]       = std::round(host_fps * 10.0) / 10.0;
	status["guest_fps"]      = std::round(guest_fps * 10.0) / 10.0;
	status["pad_reads"]      = s.pad_reads.load();
	status["input_resets"]   = s.input_resets.load();
	status["commands"]       = s.commands_run.load();
	status["shots"]          = s.shots_taken.load();
	status["trace_submits"]  = s.trace_submits.load();

	const auto* stage = s.shader_stage.load();
	if (stage != nullptr) {
		status["last_shader"] = {{"hash", Hex64(s.shader_hash.load())},
		                         {"stage", stage},
		                         {"age_ms", UptimeMs(s) - s.shader_time_ms.load()}};
	}

	nlohmann::json schedulers = nlohmann::json::array();
	{
		std::lock_guard lock(g_ticks_mutex);
		for (auto* master: g_schedulers) {
			schedulers.push_back({{"gpu_tick", master->QueryGpuTick()},
			                      {"current_tick", master->CurrentTick()}});
		}
	}
	status["schedulers"] = schedulers;

	nlohmann::json recent = nlohmann::json::array();
	const auto     cursor = s.submit_cursor.load();
	for (uint64_t i = 0; i < std::min<uint64_t>(cursor, REPORTED_SUBMITS); i++) {
		const auto& record = s.submits[(cursor - 1 - i) % RECENT_SUBMITS];
		recent.push_back({{"tick", record.tick.load()},
		                  {"op", record.op.load()},
		                  {"submit_id", record.submit_id.load()},
		                  {"args", {record.arg0.load(), record.arg1.load(), record.arg2.load(),
		                            record.arg3.load(), record.arg4.load()}}});
	}
	status["submits"]        = cursor;
	status["recent_submits"] = recent;

	const auto tmp = std::filesystem::path(s.status_path.string() + ".tmp");
	WriteText(tmp, status.dump(2) + "\n");
	std::error_code ec;
	std::filesystem::rename(tmp, s.status_path, ec);
}

void Run(State* s) {
	PostEvent(*s, {{"event", "started"}, {"build", KYTY_BUILD_LABEL}});
	for (;;) {
		const auto now = Clock::now();
		PollCommands(*s);
		FireTimedActions(*s, now);
		MaybeAutoShot(*s, now);
		ProcessDeliveries(*s);
		if (now >= s->next_heartbeat) {
			s->next_heartbeat = now + HEARTBEAT_INTERVAL;
			WriteStatus(*s, now);
		}
		DrainEvents(*s);
		std::this_thread::sleep_for(POLL_INTERVAL);
	}
}

} // namespace

bool Enabled() {
	return g_enabled.load(std::memory_order_relaxed);
}

void Start() {
	if (!Config::AutomationEnabled() || g_state != nullptr) {
		return;
	}
	auto* state                  = new State;
	state->dir                   = Config::GetAutomationDir();
	state->shots_dir             = state->dir / "shots";
	state->commands_path         = state->dir / "commands.txt";
	state->events_path           = state->dir / "events.jsonl";
	state->status_path           = state->dir / "status.json";
	state->latest_path           = state->dir / "latest.png";
	state->shot_interval_seconds = Config::GetAutomationShotInterval();
	state->next_auto_shot        = Clock::now() + std::chrono::seconds(state->shot_interval_seconds);

	Common::File::CreateDirectories(state->shots_dir);
	// A fresh log of events for this run. commands.txt is never truncated: the driver may already
	// have queued input for the game's boot.
	WriteText(state->events_path, "");
	if (!Common::File::IsFileExisting(state->commands_path)) {
		WriteText(state->commands_path, "");
	}

#if defined(__linux__)
	// Let the harness gdb-attach to a hung emulator whatever kernel.yama.ptrace_scope says.
	(void)::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

	g_state = state;
	g_enabled.store(true, std::memory_order_release);
	std::thread(Run, state).detach();
}

void NoteGuestFlip() {
	if (Enabled()) {
		g_state->guest_flips.fetch_add(1, std::memory_order_relaxed);
	}
}

void NoteHostPresent() {
	if (Enabled()) {
		g_state->host_presents.fetch_add(1, std::memory_order_relaxed);
	}
}

void NotePadRead() {
	if (Enabled()) {
		g_state->pad_reads.fetch_add(1, std::memory_order_relaxed);
	}
}

void NoteShaderCompile(uint64_t hash, const char* stage) {
	if (!Enabled()) {
		return;
	}
	auto& s = *g_state;
	s.shader_hash.store(hash, std::memory_order_relaxed);
	s.shader_time_ms.store(UptimeMs(s), std::memory_order_relaxed);
	s.shader_stage.store(stage, std::memory_order_release);
}

void NoteGpuSubmit(uint64_t tick, uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                   uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	if (!Enabled()) {
		return;
	}
	auto& s      = *g_state;
	auto& record = s.submits[s.submit_cursor.load(std::memory_order_relaxed) % RECENT_SUBMITS];
	record.tick.store(tick, std::memory_order_relaxed);
	record.submit_id.store(submit_id, std::memory_order_relaxed);
	record.arg4.store(arg4, std::memory_order_relaxed);
	record.op.store(op, std::memory_order_relaxed);
	record.arg0.store(arg0, std::memory_order_relaxed);
	record.arg1.store(arg1, std::memory_order_relaxed);
	record.arg2.store(arg2, std::memory_order_relaxed);
	record.arg3.store(arg3, std::memory_order_relaxed);
	s.submit_cursor.fetch_add(1, std::memory_order_release);
}

void NoteInputReset(const char* reason) {
	if (!Enabled()) {
		return;
	}
	g_state->input_resets.fetch_add(1, std::memory_order_relaxed);
	PostEvent(*g_state, {{"event", "input_reset"}, {"reason", reason}});
}

bool SubmitTraceEnabled() {
	return Enabled() && g_state->trace_submits.load(std::memory_order_relaxed);
}

void RegisterGpuTicks(Graphics::MasterSemaphore* master) {
	std::lock_guard lock(g_ticks_mutex);
	g_schedulers.push_back(master);
}

void UnregisterGpuTicks(Graphics::MasterSemaphore* master) {
	std::lock_guard lock(g_ticks_mutex);
	std::erase(g_schedulers, master);
}

std::vector<std::string> TakeScreenshotRequests() {
	if (!Enabled() || !g_state->shot_pending.exchange(false, std::memory_order_acq_rel)) {
		return {};
	}
	std::lock_guard          lock(g_state->queue_mutex);
	std::vector<std::string> names;
	names.swap(g_state->shot_requests);
	return names;
}

bool ScreenshotPending() {
	return Enabled() && g_state->shot_pending.load(std::memory_order_acquire);
}

void DeliverScreenshot(std::vector<std::string> names, uint32_t width, uint32_t height,
                       ShotFormat format, std::vector<uint8_t> pixels) {
	if (!Enabled()) {
		return;
	}
	State::Delivery shot;
	shot.names  = std::move(names);
	shot.width  = width;
	shot.height = height;
	shot.format = format;
	shot.pixels = std::move(pixels);
	std::lock_guard lock(g_state->queue_mutex);
	g_state->deliveries.push_back(std::move(shot));
}

void ScreenshotFailed(const std::vector<std::string>& names, const std::string& reason) {
	if (!Enabled()) {
		return;
	}
	State::Delivery shot;
	shot.names = names;
	shot.error = reason;
	std::lock_guard lock(g_state->queue_mutex);
	g_state->deliveries.push_back(std::move(shot));
}

uint32_t TakePresentStallSeconds() {
	return Enabled() ? g_state->stall_seconds.exchange(0, std::memory_order_acq_rel) : 0;
}

} // namespace Libs::Automation
