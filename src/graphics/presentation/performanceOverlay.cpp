#include "graphics/presentation/performanceOverlay.h"

#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/hostStats.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/systemOverlay.h"
#include "imgui.h"
#include "kernel/memory.h"
#include "loader/systemContent.h"

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Libs::Graphics {

namespace {

using Clock   = std::chrono::steady_clock;
using Counter = Common::DebugCounters::Counter;
using Peak    = Common::DebugCounters::Peak;

constexpr auto   REFRESH_INTERVAL  = std::chrono::seconds(1);
constexpr size_t FRAME_HISTORY     = 120;
constexpr float  FRAME_GRAPH_MS    = 50.0f;
constexpr float  FRAME_TIME_GOOD   = 17.5f;
constexpr float  FRAME_TIME_MEDIUM = 34.0f;
constexpr size_t COUNTER_COUNT     = static_cast<size_t>(Counter::Count);

// JSON names of the counters. Durations are exported in milliseconds and sizes in megabytes.
struct CounterInfo {
	const char* name;
	double      scale;
};
constexpr double                                 MS           = 1e6;
constexpr double                                 MB           = 1024.0 * 1024.0;
constexpr std::array<CounterInfo, COUNTER_COUNT> COUNTER_INFO = {{
    {"draws", 1},
    {"draw_instances", 1},
    {"indirect_draws", 1},
    {"dispatches", 1},
    {"queue_submits", 1},
    {"gpu_waits", 1},
    {"gpu_wait_ms", MS},
    {"gpu_faults", 1},
    {"shaders_compiled", 1},
    {"shader_compile_ms", MS},
    {"pipelines_created", 1},
    {"pipeline_create_ms", MS},
    {"texture_upload_mb", MB},
    {"texture_download_mb", MB},
    {"texture_evictions", 1},
    {"color_metadata_readbacks", 1},
    {"color_metadata_readback_ms", MS},
    {"color_metadata_checks", 1},
    {"buffer_upload_mb", MB},
    {"buffer_download_mb", MB},
    {"buffer_evictions", 1},
    {"flips_requested", 1},
    {"flips_rejected", 1},
    {"late_vblanks", 1},
    {"swapchain_recreations", 1},
    {"dropped_presents", 1},
    {"emulated_instructions", 1},
    {"unresolved_import_calls", 1},
    {"guest_file_read_mb", MB},
    {"guest_file_write_mb", MB},
    {"audio_underruns", 1},
    {"gpu_thread_busy_ms", MS},
    {"present_ms", MS},
    {"present_acquire_ms", MS},
    {"overlay_ms", MS},
}};
static_assert(COUNTER_INFO.back().name != nullptr, "every counter needs a JSON name");

using CounterTotals = std::array<uint64_t, COUNTER_COUNT>;

CounterTotals ReadCounters() {
	CounterTotals totals {};
	for (size_t i = 0; i < COUNTER_COUNT; i++) {
		totals[i] = Common::DebugCounters::Get(static_cast<Counter>(i));
	}
	return totals;
}

// A frame much longer than the recent average, with the counters' growth during it.
struct Hitch {
	Clock::time_point time;
	float             ms;
	CounterTotals     deltas;
};

constexpr float  HITCH_MIN_MS        = 50.0f;
constexpr float  HITCH_FACTOR        = 2.0f; // Times the recent average frame time.
constexpr size_t HITCH_HISTORY       = 32;
constexpr size_t MAX_RECORDED_HITCH  = 10000;
constexpr auto   HITCH_RECENT_WINDOW = std::chrono::seconds(10);

// Tables as wide as their columns; the default fills the window, which an auto-resizing
// window then never shrinks.
constexpr ImGuiTableFlags TABLE_FLAGS =
    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;

constexpr ImU32 GOOD    = IM_COL32(96, 220, 120, 255);
constexpr ImU32 MEDIUM  = IM_COL32(230, 195, 60, 255);
constexpr ImU32 BAD     = IM_COL32(235, 80, 70, 255);
constexpr ImU32 NEUTRAL = IM_COL32(160, 160, 160, 255);

struct FrameWindow {
	std::array<float, FRAME_HISTORY> intervals_ms {};
	size_t                           next         = 0;
	size_t                           count        = 0;
	uint32_t                         flips        = 0;
	float                            max_ms       = 0.0f;
	uint32_t                         guest_width  = 0;
	uint32_t                         guest_height = 0;
	Clock::time_point                last_flip    = {};
	Clock::time_point                window_start = {};
	bool                             recording    = false;
	std::vector<float>               recorded_ms;
	std::vector<Hitch>               recorded_hitches;
	CounterTotals                    flip_totals {}; // Counters at the last flip.
	float                            average_ms = 0.0f;
	std::deque<Hitch>                hitches;
};

constexpr size_t MAX_RECORDED_FRAMES  = size_t {1} << 20;
constexpr size_t MAX_RECORDED_SAMPLES = 3600; // One hour of refreshes.

// Shown values, recomputed once per refresh interval.
struct Snapshot {
	double                              fps          = 0.0;
	double                              frame_ms     = 0.0;
	double                              max_frame_ms = 0.0;
	double                              low_fps      = 0.0; // From the 99th percentile.
	double                              stalled_s    = 0.0;
	uint32_t                            guest_width  = 0;
	uint32_t                            guest_height = 0;
	std::array<double, COUNTER_COUNT>   rates {};
	std::array<uint64_t, COUNTER_COUNT> totals {};
	uint64_t                            shader_compile_peak_ns  = 0;
	uint64_t                            pipeline_create_peak_ns = 0;
	Common::HostStats                   host;
	std::optional<uint64_t>             device_memory;
	uint64_t                            device_budget = 0;
	std::array<GraphicContext::HeapUsage, VK_MAX_MEMORY_HEAPS> heaps {};
	uint32_t                                                   heap_count = 0;
	std::array<float, FRAME_HISTORY>                           history {};
	size_t                                                     history_count = 0;
	Clock::time_point                                          sample_time {};
	LibKernel::Memory::DebugStats                              guest_memory;
	std::deque<Hitch>                                          hitches;
};

std::mutex            g_frame_mutex;
FrameWindow           g_frames;
std::atomic<uint64_t> g_generation {0};
std::atomic<bool>     g_details {false};
std::atomic<uint64_t> g_revision {0};
std::atomic<bool>     g_record_toggle {false}; // F3 was pressed.

std::mutex                  g_host_mutex;
Common::HostStats           g_host_stats;
uint64_t                    g_host_generation = 0;
std::condition_variable_any g_sampler_wait;
std::jthread                g_sampler;

// Panel bounds, written by the present thread and hit-tested by the event thread.
std::mutex g_panel_mutex;
ImVec2     g_panel_min;
ImVec2     g_panel_max;

constexpr ImU32 SELECTION       = IM_COL32(70, 130, 230, 110);
constexpr ImU32 HOVER           = IM_COL32(255, 255, 255, 18); // Hints that lines are selectable.
constexpr auto  STATUS_DURATION = std::chrono::seconds(2);

// A line of the panel that can be selected and copied as text.
struct TextLine {
	std::string text;
	ImVec2      min;
	ImVec2      max;
	std::string field; // "Section / Label" of a row that can be recorded alone, else empty.
	std::string value;
};

// Lines are selected by index, from the anchor (where the press began) to the current line.
struct Selection {
	int  anchor   = -1;
	int  current  = -1;
	bool dragging = false;
};

// Present thread only.
std::vector<TextLine> g_lines;
std::string           g_section; // Title of the section being drawn.
Selection             g_selection;
std::string           g_status;
Clock::time_point     g_status_time {};
// Set while the sections are written to JSON instead of drawn.
nlohmann::ordered_json* g_export         = nullptr;
nlohmann::ordered_json* g_export_section = nullptr;

bool LineSelected(size_t index) {
	if (g_selection.anchor < 0) {
		return false;
	}
	const auto first = static_cast<size_t>(std::min(g_selection.anchor, g_selection.current));
	const auto last  = static_cast<size_t>(std::max(g_selection.anchor, g_selection.current));
	return index >= first && index <= last;
}

int g_hovered_line = -1; // The line under the mouse in the previous frame.

std::optional<ImU32> LineColor(size_t index) {
	if (LineSelected(index)) {
		return SELECTION;
	}
	return static_cast<int>(index) == g_hovered_line ? std::optional(HOVER) : std::nullopt;
}

// Registers the last drawn item as a selectable line outside a table.
void AddItemLine(std::string text, std::string field = {}) {
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	if (const auto color = LineColor(g_lines.size())) {
		ImGui::GetWindowDrawList()->AddRectFilled(min, max, *color);
	}
	std::string value = field.empty() ? std::string() : text;
	g_lines.push_back({std::move(text), min, max, std::move(field), std::move(value)});
}

void SetOverlayEnabled(bool enabled) {
	{
		std::scoped_lock lock(g_host_mutex);
		if (enabled) {
			std::scoped_lock frame_lock(g_frame_mutex);
			g_frames = {};
			g_generation.fetch_add(1, std::memory_order_release);
		}
		Common::DebugCounters::SetEnabled(enabled);
	}
	g_sampler_wait.notify_all();
}

bool GuestStalled(Clock::time_point now) {
	std::scoped_lock lock(g_frame_mutex);
	return now - g_frames.last_flip >= REFRESH_INTERVAL;
}

// Host statistics can block for milliseconds (IOKit, thread enumeration), so sample them off
// the present path. While the guest stops flipping (loading screens), ask for one repaint per
// interval.
void SamplerThread(const std::stop_token& stop) {
	std::optional<Common::HostStatsSampler> sampler;
	uint64_t                                sampled_generation = 0;
	std::unique_lock                        lock(g_host_mutex);
	while (!stop.stop_requested()) {
		g_sampler_wait.wait(lock, stop, [] { return Common::DebugCounters::Enabled(); });
		if (stop.stop_requested()) {
			break;
		}
		const auto generation = g_generation.load(std::memory_order_acquire);
		lock.unlock();
		if (!sampler || generation != sampled_generation) {
			sampler.emplace();
			sampled_generation = generation;
		}
		auto stats = sampler->Sample();
		lock.lock();
		g_host_stats      = std::move(stats);
		g_host_generation = generation;
		if (Common::DebugCounters::Enabled() && GuestStalled(Clock::now())) {
			g_revision.fetch_add(1, std::memory_order_acq_rel);
		}
		g_sampler_wait.wait_for(lock, stop, REFRESH_INTERVAL, [generation] {
			return !Common::DebugCounters::Enabled() ||
			       generation != g_generation.load(std::memory_order_acquire);
		});
	}
}

void EnsureSampler() {
	static std::once_flag once;
	std::call_once(once, [] { g_sampler = std::jthread(SamplerThread); });
}

ImU32 FrameTimeColor(double ms) {
	if (ms <= FRAME_TIME_GOOD) {
		return GOOD;
	}
	return ms <= FRAME_TIME_MEDIUM ? MEDIUM : BAD;
}

// Green below `warn`, yellow below `bad`, red above.
ImU32 LevelColor(double value, double warn, double bad) {
	if (value < warn) {
		return GOOD;
	}
	return value < bad ? MEDIUM : BAD;
}

ImVec4 Color(ImU32 color) {
	return ImGui::ColorConvertU32ToFloat4(color);
}

std::string Bytes(uint64_t bytes) {
	constexpr double GIB = 1024.0 * 1024.0 * 1024.0;
	constexpr double MIB = 1024.0 * 1024.0;
	char             buffer[32];
	if (static_cast<double>(bytes) >= GIB) {
		std::snprintf(buffer, sizeof(buffer), "%.2f GB", static_cast<double>(bytes) / GIB);
	} else {
		std::snprintf(buffer, sizeof(buffer), "%.0f MB", static_cast<double>(bytes) / MIB);
	}
	return buffer;
}

std::string Bytes(std::optional<uint64_t> bytes) {
	return bytes ? Bytes(*bytes) : "n/a";
}

std::string Throughput(double bytes_per_second) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.1f MB/s", bytes_per_second / (1024.0 * 1024.0));
	return buffer;
}

std::string Percent(std::optional<double> value) {
	if (!value) {
		return "n/a";
	}
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%.0f%%", *value);
	return buffer;
}

std::string_view TrimLabel(const char* label) {
	std::string_view name(label);
	name.remove_prefix(std::min(name.find_first_not_of(' '), name.size()));
	return name;
}

void ExportRow(const char* label, const std::string& value) {
	const auto  name = TrimLabel(label);
	std::string key(name);
	for (int i = 2; g_export_section->contains(key); i++) {
		key = std::string(name) + " (" + std::to_string(i) + ")";
	}
	(*g_export_section)[key] = value;
}

void Row(const char* label, const std::string& value, std::optional<ImU32> color = std::nullopt) {
	if (g_export_section != nullptr) {
		ExportRow(label, value);
		return;
	}
	ImGui::TableNextRow();
	if (const auto color = LineColor(g_lines.size())) {
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, *color);
	}
	ImGui::TableNextColumn();
	const ImVec2 min = ImGui::GetCursorScreenPos();
	ImGui::TextDisabled("%s", label);
	ImGui::TableNextColumn();
	if (color) {
		ImGui::TextColored(Color(*color), "%s", value.c_str());
	} else {
		ImGui::TextUnformatted(value.c_str());
	}
	const std::string name(TrimLabel(label));
	g_lines.push_back({std::string(label) + ": " + value, min, ImGui::GetItemRectMax(),
	                   g_section.empty() ? name : g_section + " / " + name, value});
}

[[gnu::format(printf, 1, 2)]] std::string Fmt(const char* format, ...) {
	char    buffer[128];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

bool BeginSection(const char* title) {
	if (g_export != nullptr) {
		g_export_section = &(*g_export)[title];
		return true;
	}
	ImGui::SeparatorText(title);
	AddItemLine(std::string("--- ") + title + " ---");
	g_section = title;
	return ImGui::BeginTable(title, 2, TABLE_FLAGS);
}

void EndSection() {
	if (g_export == nullptr) {
		ImGui::EndTable();
	}
}

void DrawFrameGraph(const std::array<float, FRAME_HISTORY>& history, size_t count, float width,
                    float height) {
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	auto*        draw   = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(0, 0, 0, 90));
	const auto y_of = [&](float ms) {
		return origin.y + height - std::min(ms, FRAME_GRAPH_MS) / FRAME_GRAPH_MS * height;
	};
	for (const float reference: {1000.0f / 60.0f, 1000.0f / 30.0f}) {
		draw->AddLine({origin.x, y_of(reference)}, {origin.x + width, y_of(reference)},
		              IM_COL32(255, 255, 255, 45));
	}
	const float bar = width / static_cast<float>(FRAME_HISTORY);
	for (size_t i = 0; i < count; i++) {
		const float ms = history[i];
		const float x  = origin.x + width - static_cast<float>(count - i) * bar;
		draw->AddRectFilled({x, y_of(ms)}, {x + std::max(bar - 1.0f, 1.0f), origin.y + height},
		                    FrameTimeColor(ms));
	}
	ImGui::Dummy({width, height});
}

const char* PressureName(Common::MemoryPressure pressure) {
	switch (pressure) {
		case Common::MemoryPressure::Normal: return "Normal";
		case Common::MemoryPressure::Warning: return "Warning";
		case Common::MemoryPressure::Critical: return "Critical";
		case Common::MemoryPressure::Unknown: break;
	}
	return "n/a";
}

ImU32 PressureColor(Common::MemoryPressure pressure) {
	switch (pressure) {
		case Common::MemoryPressure::Normal: return GOOD;
		case Common::MemoryPressure::Warning: return MEDIUM;
		case Common::MemoryPressure::Critical: return BAD;
		case Common::MemoryPressure::Unknown: break;
	}
	return NEUTRAL;
}

const char* ThermalName(Common::ThermalPressure thermal) {
	switch (thermal) {
		case Common::ThermalPressure::Nominal: return "Nominal";
		case Common::ThermalPressure::Moderate: return "Moderate";
		case Common::ThermalPressure::Heavy: return "Heavy (throttling)";
		case Common::ThermalPressure::Critical: return "Critical (throttling)";
		case Common::ThermalPressure::Unknown: break;
	}
	return "n/a";
}

ImU32 ThermalColor(Common::ThermalPressure thermal) {
	switch (thermal) {
		case Common::ThermalPressure::Nominal: return GOOD;
		case Common::ThermalPressure::Moderate: return MEDIUM;
		case Common::ThermalPressure::Heavy:
		case Common::ThermalPressure::Critical: return BAD;
		case Common::ThermalPressure::Unknown: break;
	}
	return NEUTRAL;
}

const char* PresentModeName(Config::PresentMode mode) {
	switch (mode) {
		case Config::PresentMode::Fifo: return "FIFO";
		case Config::PresentMode::Mailbox: return "Mailbox";
		case Config::PresentMode::Immediate: return "Immediate";
	}
	return "?";
}

double Rate(const Snapshot& shown, Counter counter) {
	return shown.rates[static_cast<size_t>(counter)];
}

uint64_t Total(const Snapshot& shown, Counter counter) {
	return shown.totals[static_cast<size_t>(counter)];
}

void TransferRow(const Snapshot& shown, const char* label, Counter upload, Counter download) {
	Row(label,
	    Throughput(Rate(shown, upload)) + " up / " + Throughput(Rate(shown, download)) + " down");
}

void GpuActivityRows(const Snapshot& shown) {
	const auto draws = Rate(shown, Counter::Draws);
	Row("Draw calls", Fmt("%.0f/frame  %.0f/s", draws / std::max(shown.fps, 1.0), draws));
	// Includes present pacing waits, so a high value alone is not a stall.
	Row("CPU waiting on GPU",
	    Fmt("%.0f ms/s  (%.0f waits/s)", Rate(shown, Counter::GpuWaitNs) / 1e6,
	        Rate(shown, Counter::GpuWaits)));
	Row("DCC/CMASK readbacks",
	    Fmt("%.0f ms/s  (%.0f/s)", Rate(shown, Counter::ColorMetadataReadbackNs) / 1e6,
	        Rate(shown, Counter::ColorMetadataReadbacks)));
	Row("DCC/CMASK GPU checks", Fmt("%.0f/s", Rate(shown, Counter::ColorMetadataChecks)));
}

void CompilationRows(const Snapshot& shown) {
	const auto row = [&](const char* label, Counter count_counter, Counter duration_counter,
	                     uint64_t peak_ns) {
		const auto count = Total(shown, count_counter);
		Row(label,
		    Fmt("%llu  (avg %.1f ms, peak %.1f ms)", static_cast<unsigned long long>(count),
		        count != 0 ? static_cast<double>(Total(shown, duration_counter)) / 1e6 /
		                         static_cast<double>(count)
		                   : 0.0,
		        static_cast<double>(peak_ns) / 1e6),
		    Rate(shown, count_counter) > 0.0 ? MEDIUM : GOOD);
	};
	row("Shader recompiler", Counter::ShadersCompiled, Counter::ShaderCompileNs,
	    shown.shader_compile_peak_ns);
	row("Pipeline compile", Counter::PipelinesCreated, Counter::PipelineCreateNs,
	    shown.pipeline_create_peak_ns);
}

double PerFrame(const Snapshot& shown, Counter counter) {
	return Rate(shown, counter) / std::max(shown.fps, 1.0);
}

double PerFrameMs(const Snapshot& shown, Counter counter) {
	return PerFrame(shown, counter) / MS;
}

// Where a frame's time goes, averaged over the last refresh interval.
void FrameSplitRows(const Snapshot& shown) {
	const auto row = [&](const char* label, Counter counter, bool bottleneck) {
		const double busy = Rate(shown, counter) / 1e9 * 100.0;
		Row(label, Fmt("%.1f ms  (%.0f%% of time)", PerFrameMs(shown, counter), busy),
		    bottleneck ? std::optional(LevelColor(busy, 80, 95)) : std::nullopt);
	};
	row("GPU thread busy", Counter::GpuThreadBusyNs, true);
	Row("Shader + pipeline compile",
	    Fmt("%.1f ms", PerFrameMs(shown, Counter::ShaderCompileNs) +
	                       PerFrameMs(shown, Counter::PipelineCreateNs)));
	row("CPU waiting on GPU", Counter::GpuWaitNs, false);
	row("Present", Counter::PresentNs, true);
	row("  Image acquire", Counter::PresentAcquireNs, false);
	row("  Overlays", Counter::OverlayNs, false);
}

// The counters most likely to explain a hitch, in that order.
std::string HitchCause(const Hitch& hitch) {
	const auto delta = [&](Counter counter) {
		return static_cast<double>(hitch.deltas[static_cast<size_t>(counter)]);
	};
	std::vector<std::string> causes;
	const auto               add = [&](double value, double min, const char* format) {
		if (value >= min && causes.size() < 3) {
			causes.push_back(Fmt(format, value));
		}
	};
	add(delta(Counter::ShadersCompiled), 1, "%.0f shaders");
	add(delta(Counter::PipelinesCreated), 1, "%.0f pipelines");
	add(delta(Counter::GpuWaitNs) / MS, 5, "%.0f ms GPU wait");
	add(delta(Counter::ColorMetadataReadbackNs) / MS, 1, "%.0f ms readbacks");
	add((delta(Counter::TextureUploadBytes) + delta(Counter::BufferUploadBytes)) / MB, 8,
	    "%.0f MB uploads");
	add(delta(Counter::GuestFileReadBytes) / MB, 1, "%.0f MB file reads");
	add(delta(Counter::SwapchainRecreations), 1, "%.0f swapchain rebuilds");
	add(delta(Counter::EmulatedInstructions), 100, "%.0f emulated instrs");
	if (causes.empty()) {
		return "no counter stands out";
	}
	std::string text = causes.front();
	for (size_t i = 1; i < causes.size(); i++) {
		text += ", " + causes[i];
	}
	return text;
}

void HitchRows(const Snapshot& shown) {
	const auto recent =
	    std::count_if(shown.hitches.begin(), shown.hitches.end(), [&](const Hitch& hitch) {
		    return shown.sample_time - hitch.time < HITCH_RECENT_WINDOW;
	    });
	Row("Hitches (10 s)", Fmt("%lld", static_cast<long long>(recent)), recent > 0 ? MEDIUM : GOOD);
	if (!shown.hitches.empty()) {
		const auto& last = shown.hitches.back();
		Row("Last hitch",
		    Fmt("%.0f ms, %.0f s ago", last.ms,
		        std::chrono::duration<double>(shown.sample_time - last.time).count()),
		    FrameTimeColor(last.ms));
		Row("  During it", HitchCause(last));
	}
}

// Stats of a saved recording to compare the live ones with, loaded from the baseline file.
std::optional<nlohmann::ordered_json> g_baseline; // Present thread only.

std::optional<double> BaselineValue(const char* group, const std::string& key) {
	if (!g_baseline) {
		return std::nullopt;
	}
	const auto values = g_baseline->find(group);
	if (values == g_baseline->end() || !values->is_object()) {
		return std::nullopt;
	}
	const auto value = values->find(key);
	return value != values->end() && value->is_number() ? std::optional(value->get<double>())
	                                                    : std::nullopt;
}

double BaselineNumber(const char* group, const char* key) {
	return BaselineValue(group, key).value_or(0.0);
}

// "live vs baseline (+x%)", colored by whether the change is for the better.
void CompareRow(const char* label, double live, double base, const char* unit,
                bool higher_is_better) {
	const double change = base != 0.0 ? (live - base) / base * 100.0 : 0.0;
	const double better = higher_is_better ? change : -change;
	Row(label, Fmt("%.1f vs %.1f %s  (%+.0f%%)", live, base, unit, change),
	    better >= -2.0   ? GOOD
	    : better >= -5.0 ? MEDIUM
	                     : BAD);
}

void BaselineRows(const Snapshot& shown) {
	CompareRow("FPS", shown.fps, BaselineNumber("summary", "avg_fps"), "", true);
	CompareRow("1% low", shown.low_fps, BaselineNumber("summary", "one_percent_low_fps"), "FPS",
	           true);
	const auto per_frame = [&](const char* label, Counter counter) {
		const auto& info = COUNTER_INFO[static_cast<size_t>(counter)];
		CompareRow(label, PerFrame(shown, counter) / info.scale,
		           BaselineNumber("per_frame", info.name), info.scale == MS ? "ms" : "", false);
	};
	per_frame("GPU thread busy", Counter::GpuThreadBusyNs);
	per_frame("Present", Counter::PresentNs);
	per_frame("CPU waiting on GPU", Counter::GpuWaitNs);
	per_frame("Draw calls", Counter::Draws);
}

void Refresh(Snapshot& shown, const GraphicContext& graphics, Clock::time_point now,
             uint64_t generation) {
	const double seconds = shown.sample_time == Clock::time_point {}
	                           ? 0.0
	                           : std::chrono::duration<double>(now - shown.sample_time).count();
	for (size_t i = 0; i < COUNTER_COUNT; i++) {
		const auto value = Common::DebugCounters::Get(static_cast<Counter>(i));
		shown.rates[i] =
		    seconds > 0.0 ? static_cast<double>(value - shown.totals[i]) / seconds : 0.0;
		shown.totals[i] = value;
	}
	shown.shader_compile_peak_ns  = Common::DebugCounters::Get(Peak::ShaderCompileNs);
	shown.pipeline_create_peak_ns = Common::DebugCounters::Get(Peak::PipelineCreateNs);
	shown.sample_time             = now;
	{
		std::scoped_lock lock(g_host_mutex);
		if (g_host_generation == generation) {
			shown.host = g_host_stats;
		}
	}
	shown.heap_count = graphics.GetHeapUsage(shown.heaps);
	shown.device_memory =
	    graphics.CanReportMemoryUsage() ? std::optional<uint64_t>(0) : std::nullopt;
	shown.device_budget = 0;
	const bool discrete =
	    graphics.GetPhysicalDeviceProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	for (uint32_t i = 0; i < shown.heap_count; i++) {
		const auto& heap = shown.heaps[i];
		if (shown.device_memory && (!discrete || heap.device_local)) {
			*shown.device_memory += heap.usage;
			shown.device_budget += heap.budget;
		}
	}
	if (g_details.load(std::memory_order_acquire)) {
		shown.guest_memory = LibKernel::Memory::GetDebugStats();
	}
}

void DrawCompact(const Snapshot& shown) {
	if (!ImGui::BeginTable("##performance", 2, TABLE_FLAGS)) {
		return;
	}
	const auto& host = shown.host;
	if (shown.max_frame_ms > 0.0) {
		Row("Worst frame", Fmt("%.1f ms", shown.max_frame_ms), FrameTimeColor(shown.max_frame_ms));
	}
	Row("CPU", host.process_cpu_percent
	               ? Fmt("%.0f%%  of %u threads", *host.process_cpu_percent, host.logical_cores)
	               : std::string("n/a"));
	Row("GPU", Percent(host.gpu_percent));
	Row("GPU thread / present",
	    Fmt("%.1f / %.1f ms per frame", PerFrameMs(shown, Counter::GpuThreadBusyNs),
	        PerFrameMs(shown, Counter::PresentNs)));
	GpuActivityRows(shown);
	CompilationRows(shown);
	HitchRows(shown);
	if (g_baseline) {
		CompareRow("vs baseline", shown.fps, BaselineNumber("summary", "avg_fps"), "FPS", true);
	}
	if (shown.device_memory) {
		Row("GPU memory", Bytes(*shown.device_memory) + " / " + Bytes(shown.device_budget));
	}
	TransferRow(shown, "Texture transfers", Counter::TextureUploadBytes,
	            Counter::TextureDownloadBytes);
	TransferRow(shown, "Buffer transfers", Counter::BufferUploadBytes,
	            Counter::BufferDownloadBytes);
	Row("Emulator RAM", Bytes(host.process_memory));
	if (host.system_memory_used && host.system_memory_total) {
		Row("System RAM",
		    Bytes(*host.system_memory_used) + " / " + Bytes(*host.system_memory_total));
	}
	ImGui::EndTable();
}

void DrawFramesAndCpu(const Snapshot& shown, vk::Extent2D extent) {
	const auto& host = shown.host;
	if (BeginSection("Frames")) {
		if (shown.low_fps > 0.0) {
			Row("1% low", Fmt("%.1f FPS", shown.low_fps), FrameTimeColor(1000.0 / shown.low_fps));
		}
		Row("Worst frame", Fmt("%.1f ms", shown.max_frame_ms), FrameTimeColor(shown.max_frame_ms));
		Row("Guest output", shown.guest_width != 0
		                        ? Fmt("%ux%u", shown.guest_width, shown.guest_height)
		                        : std::string("n/a"));
		Row("Window", Fmt("%ux%u  %s  %u Hz", extent.width, extent.height,
		                  PresentModeName(Config::GetPresentMode()), Config::GetVblankFrequency()));
		Row("Flips", Fmt("%.0f/s requested", Rate(shown, Counter::FlipsRequested)));
		const auto rejected = Rate(shown, Counter::FlipsRejected);
		Row("Flip queue full", Fmt("%.0f/s", rejected), rejected > 0.0 ? BAD : GOOD);
		const auto late = Rate(shown, Counter::LateVblanks);
		Row("Late vblanks", Fmt("%.0f/s", late), LevelColor(late, 1.0, 10.0));
		Row("Swapchain rebuilds",
		    Fmt("%llu  (dropped %llu)",
		        static_cast<unsigned long long>(Total(shown, Counter::SwapchainRecreations)),
		        static_cast<unsigned long long>(Total(shown, Counter::DroppedPresents))));
		EndSection();
	}
	if (BeginSection("Frame split (per frame)")) {
		FrameSplitRows(shown);
		EndSection();
	}
	if (BeginSection("CPU")) {
		Row("Emulator",
		    host.process_cpu_percent
		        ? Fmt("%.0f%% of %u threads  (%.1f cores)", *host.process_cpu_percent,
		              host.logical_cores, *host.process_cpu_percent * host.logical_cores / 100.0)
		        : std::string("n/a"));
		Row("Machine", Percent(host.system_cpu_percent),
		    host.system_cpu_percent ? std::optional(LevelColor(*host.system_cpu_percent, 70, 90))
		                            : std::nullopt);
		if (host.thread_count) {
			Row("Host threads", Fmt("%u", *host.thread_count));
		}
		for (const auto& thread: host.busiest_threads) {
			Row(Fmt("  %.24s", thread.name.c_str()).c_str(),
			    Fmt("%.0f%%  (%.1f ms/frame)", thread.cpu_percent,
			        thread.cpu_percent / 100.0 * shown.frame_ms),
			    LevelColor(thread.cpu_percent, 70, 95));
		}
		if (host.context_switches_per_s) {
			Row("Context switches", Fmt("%.0f/s", *host.context_switches_per_s));
		}
		if (host.page_faults_per_s) {
			Row("Page faults", Fmt("%.0f/s  (%.0f from disk)", *host.page_faults_per_s,
			                       host.page_ins_per_s.value_or(0.0)));
		}
		Row("Thermal", ThermalName(host.thermal_pressure), ThermalColor(host.thermal_pressure));
		const auto emulated = Rate(shown, Counter::EmulatedInstructions);
		Row("Emulated instrs", Fmt("%.0f/s", emulated), LevelColor(emulated, 1000, 100000));
		const auto unresolved = Rate(shown, Counter::UnresolvedImportCalls);
		Row("Missing HLE calls",
		    Fmt("%.0f/s  (%llu total)", unresolved,
		        static_cast<unsigned long long>(Total(shown, Counter::UnresolvedImportCalls))),
		    unresolved > 0.0 ? MEDIUM : GOOD);
		EndSection();
	}
}

std::string GpuName(const Common::HostStats& host, const GraphicContext& graphics) {
	if (host.gpu_model.empty()) {
		return graphics.GetPhysicalDeviceProperties().deviceName.data();
	}
	return host.gpu_cores ? Fmt("%s (%u cores)", host.gpu_model.c_str(), *host.gpu_cores)
	                      : host.gpu_model;
}

void DrawGpu(const Snapshot& shown, const GraphicContext& graphics) {
	const auto& host = shown.host;
	if (BeginSection("GPU")) {
		Row("Device", GpuName(host, graphics));
		Row("Utilization", Percent(host.gpu_percent),
		    host.gpu_percent ? std::optional(LevelColor(*host.gpu_percent, 80, 95)) : std::nullopt);
		if (host.gpu_renderer_percent || host.gpu_tiler_percent) {
			Row("Renderer / tiler",
			    Percent(host.gpu_renderer_percent) + " / " + Percent(host.gpu_tiler_percent));
		}
		if (host.gpu_memory_in_use) {
			Row("Driver memory", Bytes(*host.gpu_memory_in_use) + " in use / " +
			                         Bytes(host.gpu_memory_allocated) + " allocated");
		}
		if (host.gpu_recoveries) {
			Row("GPU resets", Fmt("%llu", static_cast<unsigned long long>(*host.gpu_recoveries)),
			    *host.gpu_recoveries > 0 ? BAD : GOOD);
		}
		const double fps = std::max(shown.fps, 1.0);
		GpuActivityRows(shown);
		Row("Instances", Fmt("%.0f/frame", Rate(shown, Counter::DrawInstances) / fps));
		Row("Indirect draws", Fmt("%.0f/s", Rate(shown, Counter::IndirectDraws)));
		Row("Dispatches", Fmt("%.0f/frame  %.0f/s", Rate(shown, Counter::Dispatches) / fps,
		                      Rate(shown, Counter::Dispatches)));
		Row("Queue submits", Fmt("%.0f/s", Rate(shown, Counter::QueueSubmits)));
		const auto faults = Rate(shown, Counter::GpuFaults);
		Row("Write-tracking faults", Fmt("%.0f/s", faults), LevelColor(faults, 20000, 100000));
		CompilationRows(shown);
		EndSection();
	}
	if (BeginSection("GPU memory")) {
		if (shown.device_memory) {
			const double share = shown.device_budget != 0
			                         ? static_cast<double>(*shown.device_memory) /
			                               static_cast<double>(shown.device_budget) * 100.0
			                         : 0.0;
			Row("Vulkan budget", Bytes(*shown.device_memory) + " / " + Bytes(shown.device_budget),
			    LevelColor(share, 80, 95));
		}
		for (size_t heap = 0; heap < shown.heap_count; heap++) {
			const auto& usage = shown.heaps[heap];
			Row(Fmt("  Heap %zu%s", heap, usage.device_local ? " (device)" : "").c_str(),
			    Bytes(usage.usage) + " / " + Bytes(usage.budget) +
			        Fmt("  %u allocs", usage.allocations));
		}
		TransferRow(shown, "Texture transfers", Counter::TextureUploadBytes,
		            Counter::TextureDownloadBytes);
		Row("Texture evictions", Fmt("%.0f/s", Rate(shown, Counter::TextureEvictions)));
		TransferRow(shown, "Buffer transfers", Counter::BufferUploadBytes,
		            Counter::BufferDownloadBytes);
		Row("Buffer evictions", Fmt("%.0f/s", Rate(shown, Counter::BufferEvictions)));
		EndSection();
	}
}

void DrawMemoryAndIo(const Snapshot& shown) {
	const auto& host  = shown.host;
	const auto& guest = shown.guest_memory;
	if (BeginSection("PS5 memory")) {
		const auto direct_used = guest.direct_allocated + guest.pooled_allocated;
		Row("Direct", Bytes(direct_used) + " / " + Bytes(guest.direct_total),
		    LevelColor(guest.direct_total != 0 ? static_cast<double>(direct_used) /
		                                             static_cast<double>(guest.direct_total) * 100.0
		                                       : 0.0,
		               85, 97));
		Row("  Mapped", Bytes(guest.direct_mapped));
		Row("  Pool",
		    Bytes(guest.pooled_allocated) + " (" + Bytes(guest.pool_committed) + " committed)");
		Row("Flexible", Bytes(guest.flexible_used) + " / " + Bytes(guest.flexible_total));
		Row("Extended", Bytes(guest.automatic_allocated));
		// Past its total a side sees no page-table entries available.
		const auto percent = std::max(guest.cpu_page_entries * 100 / guest.cpu_page_entries_total,
		                              guest.gpu_page_entries * 100 / guest.gpu_page_entries_total);
		Row("Page table (2 MB)",
		    Fmt("CPU %llu / %llu  GPU %llu / %llu",
		        static_cast<unsigned long long>(guest.cpu_page_entries),
		        static_cast<unsigned long long>(guest.cpu_page_entries_total),
		        static_cast<unsigned long long>(guest.gpu_page_entries),
		        static_cast<unsigned long long>(guest.gpu_page_entries_total)),
		    LevelColor(static_cast<double>(percent), 90, 100));
		EndSection();
	}
	if (BeginSection("Emulator process")) {
		Row("Footprint", Bytes(host.process_memory));
		Row("Resident", Bytes(host.process_resident));
		if (host.process_compressed) {
			Row("Compressed / swapped", Bytes(*host.process_compressed),
			    *host.process_compressed > (1ull << 30u) ? MEDIUM : GOOD);
		}
		Row("File-backed", Bytes(host.process_file_backed));
		Row("Address space", Bytes(host.process_virtual));
		EndSection();
	}
	if (BeginSection("System memory")) {
		if (host.system_memory_used && host.system_memory_total) {
			Row("Used", Bytes(*host.system_memory_used) + " / " + Bytes(*host.system_memory_total));
		}
		if (host.system_wired) {
			Row("Wired", Bytes(*host.system_wired));
		}
		if (host.system_compressed) {
			Row("Compressed", Bytes(*host.system_compressed));
		}
		Row("Cached files", Bytes(host.system_cached));
		Row("Free", Bytes(host.system_free));
		if (host.swap_used) {
			Row("Swap", Bytes(*host.swap_used) + " / " + Bytes(host.swap_total),
			    *host.swap_used > (4ull << 30u) ? MEDIUM : GOOD);
		}
		Row("Pressure", PressureName(host.memory_pressure), PressureColor(host.memory_pressure));
		EndSection();
	}
	if (BeginSection("I/O & audio")) {
		Row("Disk read / write", Throughput(host.disk_read_per_s.value_or(0.0)) + " / " +
		                             Throughput(host.disk_write_per_s.value_or(0.0)));
		Row("Guest file read / write", Throughput(Rate(shown, Counter::GuestFileReadBytes)) +
		                                   " / " +
		                                   Throughput(Rate(shown, Counter::GuestFileWriteBytes)));
		const auto underruns = Rate(shown, Counter::AudioUnderruns);
		Row("Audio underruns",
		    Fmt("%.0f/s  (%llu total)", underruns,
		        static_cast<unsigned long long>(Total(shown, Counter::AudioUnderruns))),
		    underruns > 0.0 ? BAD : GOOD);
		EndSection();
	}
	if (BeginSection("Hitches")) {
		HitchRows(shown);
		EndSection();
	}
	if (g_baseline && BeginSection("vs baseline")) {
		BaselineRows(shown);
		EndSection();
	}
}

double Round2(double value) {
	return std::round(value * 100.0) / 100.0;
}

// Counter values by JSON name, divided by `divisor`, leaving out zeros when `skip_zero`.
nlohmann::ordered_json CounterJson(const CounterTotals& values, double divisor, bool skip_zero) {
	auto json = nlohmann::ordered_json::object();
	for (size_t i = 0; i < COUNTER_COUNT; i++) {
		if (!skip_zero || values[i] != 0) {
			json[COUNTER_INFO[i].name] =
			    Round2(static_cast<double>(values[i]) / COUNTER_INFO[i].scale / divisor);
		}
	}
	return json;
}

nlohmann::ordered_json HitchJson(const Hitch& hitch, const char* time_key, double time_s) {
	nlohmann::ordered_json json;
	json[time_key]   = Round2(time_s);
	json["frame_ms"] = Round2(hitch.ms);
	json["during"]   = CounterJson(hitch.deltas, 1.0, true);
	return json;
}

std::string Dump(const nlohmann::ordered_json& json) {
	return json.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

// Writes the shown values into `json`, with the recent frame times when `history` is set.
void ExportSnapshot(nlohmann::ordered_json& json, const Snapshot& shown,
                    const GraphicContext& graphics, vk::Extent2D extent, bool history) {
	// The compact panel shows a subset and does not sample guest memory, so export every
	// section of the detailed one.
	Snapshot full               = shown;
	full.guest_memory           = LibKernel::Memory::GetDebugStats();
	json["fps"]                 = Round2(shown.fps);
	json["frame_ms"]            = Round2(shown.frame_ms);
	json["worst_frame_ms"]      = Round2(shown.max_frame_ms);
	json["one_percent_low_fps"] = Round2(shown.low_fps);
	json["no_frames_s"]         = Round2(shown.stalled_s);
	if (history) {
		std::vector<double> frame_times;
		for (size_t i = 0; i < shown.history_count; i++) {
			frame_times.push_back(Round2(shown.history[i]));
		}
		json["frame_times_ms"] = frame_times;
		auto hitches           = nlohmann::ordered_json::array();
		for (const auto& hitch: shown.hitches) {
			hitches.push_back(
			    HitchJson(hitch, "age_s",
			              std::chrono::duration<double>(shown.sample_time - hitch.time).count()));
		}
		json["recent_hitches"] = std::move(hitches);
	}
	auto sections = nlohmann::ordered_json::object();
	g_export      = &sections;
	DrawFramesAndCpu(full, extent);
	DrawGpu(full, graphics);
	DrawMemoryAndIo(full);
	g_export         = nullptr;
	g_export_section = nullptr;
	json["sections"] = std::move(sections);
}

void SetStatus(std::string text) {
	g_status      = std::move(text);
	g_status_time = Clock::now();
}

std::filesystem::path RecordingsDirectory() {
	const char* base = SDL_GetBasePath();
	return (base != nullptr ? std::filesystem::path(base) : std::filesystem::current_path()) /
	       "perf_recordings";
}

std::string TitleId() {
	std::string id;
	return Loader::SystemContentParamSfoGetString("TITLE_ID", &id) && !id.empty() ? id : "unknown";
}

std::filesystem::path BaselinePath() {
	return RecordingsDirectory() / ("baseline_" + TitleId() + ".json");
}

// UTC, sortable, and valid in file names.
std::string Timestamp() {
	return fmt::format("{:%Y%m%d-%H%M%S}",
	                   std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

// Writes off the present thread, so saving a long recording does not stall a frame.
void WriteFileAsync(std::filesystem::path path, std::string text) {
	std::thread([path = std::move(path), text = std::move(text)] {
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
			LOGF("[Overlay] could not write %s\n", Common::PathToString(path).c_str());
		}
	}).detach();
}

// Copies `json` to the clipboard, or saves it to a new file in the recordings folder.
void Deliver(const nlohmann::ordered_json& json, bool save, const std::string& what) {
	if (!save) {
		SetHostClipboardText(Dump(json));
		SetStatus("Copied " + what);
		return;
	}
	const auto name = TitleId() + "_" + Timestamp() + ".json";
	WriteFileAsync(RecordingsDirectory() / name, Dump(json));
	SetStatus("Saved perf_recordings/" + name);
}

// The summary of the last finished recording of all stats, which can become the baseline.
std::optional<nlohmann::ordered_json> g_last_recording; // Present thread only.

void LoadBaseline() {
	std::ifstream file(BaselinePath(), std::ios::binary);
	if (!file) {
		return;
	}
	auto json = nlohmann::ordered_json::parse(file, nullptr, false);
	if (json.is_object()) {
		g_baseline = std::move(json);
	}
}

void SetBaseline() {
	g_baseline = std::move(*g_last_recording);
	g_last_recording.reset();
	WriteFileAsync(BaselinePath(), Dump(*g_baseline));
	SetStatus("Baseline saved");
}

// A recording keeps every frame time and one snapshot per refresh until it is stopped. A
// recording of selected fields keeps only those lines' values per refresh.
struct Recording {
	bool                     active = false;
	Clock::time_point        start {};
	nlohmann::ordered_json   samples;
	std::vector<std::string> fields;
	bool                     fields_pending = false; // A refresh happened; sample drawn lines.
	CounterTotals            start_totals {};
	nlohmann::ordered_json   trigger_events = nlohmann::ordered_json::array();
};

Recording g_recording; // Present thread only.

void StartRecording(Clock::time_point now, std::vector<std::string> fields = {}) {
	{
		std::scoped_lock lock(g_frame_mutex);
		g_frames.recording = fields.empty();
		g_frames.recorded_ms.clear();
		g_frames.recorded_hitches.clear();
	}
	g_recording = {true,  now,           nlohmann::ordered_json::array(), std::move(fields),
	               false, ReadCounters()};
}

double RecordingSeconds(Clock::time_point now) {
	return std::chrono::duration<double>(now - g_recording.start).count();
}

// Names a field recording for buttons and status messages.
std::string FieldsName(const std::vector<std::string>& fields) {
	return fields.size() == 1 ? fields.front() : Fmt("%zu fields", fields.size());
}

void RecordFieldsSample(Clock::time_point now) {
	nlohmann::ordered_json sample;
	sample["t_s"] = Round2(RecordingSeconds(now));
	for (const auto& field: g_recording.fields) {
		const auto line = std::find_if(g_lines.begin(), g_lines.end(),
		                               [&](const TextLine& line) { return line.field == field; });
		// Null while the line is not shown, e.g. after switching to the compact panel.
		sample[field] = line != g_lines.end() ? nlohmann::ordered_json(line->value) : nullptr;
	}
	g_recording.samples.push_back(std::move(sample));
}

void StopFieldsRecording(Clock::time_point now, bool save) {
	const double           duration = RecordingSeconds(now);
	nlohmann::ordered_json json;
	json["title_id"]   = TitleId();
	json["fields"]     = g_recording.fields;
	json["duration_s"] = Round2(duration);
	json["samples"]    = std::move(g_recording.samples);
	if (!g_recording.trigger_events.empty()) {
		json["trigger_events"] = std::move(g_recording.trigger_events);
	}
	const auto what = Fmt("%.0f s of %s", duration, FieldsName(g_recording.fields).c_str());
	g_recording     = {};
	Deliver(json, save, what);
}

// The recordable fields of the selected lines, once the selection is finished.
std::vector<std::string> SelectedFields() {
	std::vector<std::string> fields;
	if (g_selection.dragging) {
		return fields;
	}
	for (size_t i = 0; i < g_lines.size(); i++) {
		const auto& field = g_lines[i].field;
		if (LineSelected(i) && !field.empty() &&
		    std::find(fields.begin(), fields.end(), field) == fields.end()) {
			fields.push_back(field);
		}
	}
	return fields;
}

void RecordSample(const Snapshot& shown, const GraphicContext& graphics, vk::Extent2D extent,
                  Clock::time_point now) {
	nlohmann::ordered_json sample;
	sample["t_s"] = Round2(std::chrono::duration<double>(now - g_recording.start).count());
	ExportSnapshot(sample, shown, graphics, extent, false);
	g_recording.samples.push_back(std::move(sample));
}

// Ends the recording and copies or saves it with a summary of all its frames.
void StopRecording(Clock::time_point now, bool save) {
	if (!g_recording.fields.empty()) {
		StopFieldsRecording(now, save);
		return;
	}
	std::vector<float> frames;
	std::vector<Hitch> hitches;
	{
		std::scoped_lock lock(g_frame_mutex);
		g_frames.recording = false;
		frames.swap(g_frames.recorded_ms);
		hitches.swap(g_frames.recorded_hitches);
	}
	const auto    end_totals = ReadCounters();
	CounterTotals deltas {};
	for (size_t i = 0; i < COUNTER_COUNT; i++) {
		deltas[i] = end_totals[i] - g_recording.start_totals[i];
	}
	const double duration = std::chrono::duration<double>(now - g_recording.start).count();
	double       total_ms = 0.0;
	for (const float ms: frames) {
		total_ms += ms;
	}
	nlohmann::ordered_json summary;
	summary["frames"] = frames.size();
	if (!frames.empty()) {
		const double average = total_ms / static_cast<double>(frames.size());
		auto         sorted  = frames;
		const auto   low = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() * 99 / 100);
		std::nth_element(sorted.begin(), low, sorted.end());
		summary["avg_fps"]             = Round2(average > 0.0 ? 1000.0 / average : 0.0);
		summary["avg_frame_ms"]        = Round2(average);
		summary["one_percent_low_fps"] = Round2(*low > 0.0f ? 1000.0 / *low : 0.0);
		summary["worst_frame_ms"]      = Round2(*std::max_element(frames.begin(), frames.end()));
	}
	std::vector<double> frame_times;
	frame_times.reserve(frames.size());
	for (const float ms: frames) {
		frame_times.push_back(Round2(ms));
	}
	// The part a baseline keeps.
	nlohmann::ordered_json result;
	result["title_id"]    = TitleId();
	result["recorded_at"] = Timestamp();
	result["duration_s"]  = Round2(duration);
	result["summary"]     = std::move(summary);
	result["per_frame"] =
	    CounterJson(deltas, std::max(static_cast<double>(frames.size()), 1.0), false);

	nlohmann::ordered_json json = result;
	if (g_baseline) {
		auto vs = nlohmann::ordered_json::object();
		for (const char* group: {"summary", "per_frame"}) {
			for (const auto& [key, value]: result[group].items()) {
				const auto base = BaselineValue(group, key);
				if (value.is_number() && base) {
					vs[group][key] = Round2(value.get<double>() - *base);
				}
			}
		}
		json["change_vs_baseline"] = std::move(vs);
		json["baseline"]           = *g_baseline;
	}
	auto hitch_json = nlohmann::ordered_json::array();
	for (const auto& hitch: hitches) {
		hitch_json.push_back(HitchJson(
		    hitch, "t_s", std::chrono::duration<double>(hitch.time - g_recording.start).count()));
	}
	json["hitches"] = std::move(hitch_json);
	if (!g_recording.trigger_events.empty()) {
		json["trigger_events"] = std::move(g_recording.trigger_events);
	}
	json["frame_times_ms"] = std::move(frame_times);
	json["samples"]        = std::move(g_recording.samples);
	g_recording            = {};
	g_last_recording       = std::move(result);
	Deliver(json, save, Fmt("%.0f s recording", duration));
}

// Fires when a field's shown value crosses a threshold, keeping a snapshot of all stats. Fields
// change once per refresh, so a trigger sees one value per second.
struct Trigger {
	std::string field;
	float       threshold = 0.0f;
	bool        above     = true;
	bool        armed     = true; // Fires again once the value is back on the other side.
	uint32_t    fired     = 0;
};

struct TriggerEvent {
	Clock::time_point      time;
	nlohmann::ordered_json json;
};

constexpr size_t MAX_TRIGGERS                = 8;
constexpr size_t TRIGGER_EVENT_HISTORY       = 32;
constexpr size_t MAX_RECORDED_TRIGGER_EVENTS = 1000;

// Present thread only.
std::vector<Trigger>     g_triggers;
std::deque<TriggerEvent> g_trigger_events;
bool                     g_triggers_pending = false; // A refresh happened; check drawn lines.

const TextLine* FindLine(const std::string& field) {
	const auto line = std::find_if(g_lines.begin(), g_lines.end(),
	                               [&](const TextLine& line) { return line.field == field; });
	return line != g_lines.end() ? &*line : nullptr;
}

// The first number of a shown value, e.g. 87 in "87%" or 12.5 in "12.5 ms  (3 waits/s)".
std::optional<double> FirstNumber(const std::string& text) {
	const auto start = text.find_first_of("-.0123456789");
	if (start == std::string::npos) {
		return std::nullopt;
	}
	const char* begin = text.c_str() + start;
	char*       end   = nullptr;
	const auto  value = std::strtod(begin, &end);
	return end != begin ? std::optional(value) : std::nullopt;
}

// `json` with `time_key` first.
nlohmann::ordered_json WithTime(const char* time_key, double seconds,
                                const nlohmann::ordered_json& json) {
	nlohmann::ordered_json result;
	result[time_key] = Round2(seconds);
	result.update(json);
	return result;
}

void CheckTriggers(const Snapshot& shown, const GraphicContext& graphics, vk::Extent2D extent,
                   Clock::time_point now) {
	for (auto& trigger: g_triggers) {
		const auto* line  = FindLine(trigger.field);
		const auto  value = line != nullptr ? FirstNumber(line->value) : std::nullopt;
		if (!value) {
			continue;
		}
		const bool met = trigger.above ? *value > trigger.threshold : *value < trigger.threshold;
		if (!met) {
			trigger.armed = true;
			continue;
		}
		if (!trigger.armed) {
			continue;
		}
		trigger.armed = false;
		trigger.fired++;
		nlohmann::ordered_json event;
		event["field"]     = trigger.field;
		event["condition"] = Fmt("%s %.2f", trigger.above ? ">" : "<", trigger.threshold);
		event["value"]     = line->value;
		auto snapshot      = nlohmann::ordered_json::object();
		ExportSnapshot(snapshot, shown, graphics, extent, true);
		event["snapshot"] = std::move(snapshot);
		if (g_recording.active && g_recording.trigger_events.size() < MAX_RECORDED_TRIGGER_EVENTS) {
			g_recording.trigger_events.push_back(WithTime("t_s", RecordingSeconds(now), event));
		}
		if (g_trigger_events.size() == TRIGGER_EVENT_HISTORY) {
			g_trigger_events.pop_front();
		}
		g_trigger_events.push_back({now, std::move(event)});
		SetStatus("Triggered: " + trigger.field);
	}
}

nlohmann::ordered_json TriggersJson(Clock::time_point now) {
	nlohmann::ordered_json json;
	auto                   triggers = nlohmann::ordered_json::array();
	for (const auto& trigger: g_triggers) {
		nlohmann::ordered_json item;
		item["field"]     = trigger.field;
		item["condition"] = Fmt("%s %.2f", trigger.above ? ">" : "<", trigger.threshold);
		item["fired"]     = trigger.fired;
		triggers.push_back(std::move(item));
	}
	auto events = nlohmann::ordered_json::array();
	for (const auto& event: g_trigger_events) {
		events.push_back(
		    WithTime("age_s", std::chrono::duration<double>(now - event.time).count(), event.json));
	}
	json["triggers"]       = std::move(triggers);
	json["trigger_events"] = std::move(events);
	return json;
}

void AddTrigger(const std::string& field) {
	const auto* line  = FindLine(field);
	const auto  value = line != nullptr ? FirstNumber(line->value) : std::nullopt;
	// Start above the current value, so the trigger waits for a rise.
	const float threshold = value && *value != 0.0 ? static_cast<float>(*value * 1.5) : 1.0f;
	g_triggers.push_back({field, threshold});
}

void DrawTriggers(float scale) {
	ImGui::SeparatorText("Triggers");
	for (size_t i = 0; i < g_triggers.size();) {
		auto& trigger = g_triggers[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::TextUnformatted(trigger.field.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton(trigger.above ? ">" : "<")) {
			trigger.above = !trigger.above;
			trigger.armed = true;
		}
		ImGui::SetItemTooltip("Fires when the value goes above or below the threshold");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f * scale);
		// Keyboard input does not reach the overlay, so the threshold is dragged.
		if (ImGui::DragFloat("##threshold", &trigger.threshold,
		                     std::max(std::abs(trigger.threshold) * 0.01f, 0.01f), 0.0f, 0.0f,
		                     "%.2f", ImGuiSliderFlags_NoInput)) {
			trigger.armed = true;
		}
		ImGui::SetItemTooltip("Drag to change the threshold");
		ImGui::SameLine();
		ImGui::TextDisabled("fired %u", trigger.fired);
		ImGui::SameLine();
		const bool remove = ImGui::SmallButton("x");
		ImGui::PopID();
		if (remove) {
			g_triggers.erase(g_triggers.begin() + static_cast<std::ptrdiff_t>(i));
		} else {
			i++;
		}
	}
}

void CopySelection() {
	std::string text;
	int         count = 0;
	for (size_t i = 0; i < g_lines.size(); i++) {
		if (LineSelected(i)) {
			text += (count++ == 0 ? "" : "\n") + g_lines[i].text;
		}
	}
	SetHostClipboardText(std::move(text));
	SetStatus(count == 1 ? std::string("Copied 1 line") : Fmt("Copied %d lines", count));
}

int LineAt(ImVec2 point) {
	const ImVec2 slack = ImGui::GetStyle().ItemSpacing;
	for (size_t i = 0; i < g_lines.size(); i++) {
		const auto& line = g_lines[i];
		if (point.y >= line.min.y && point.y < line.max.y + slack.y &&
		    point.x >= line.min.x - slack.x && point.x <= line.max.x + slack.x) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// Pressing on a line and dragging selects the lines in between; releasing copies them.
void UpdateSelection() {
	const auto count = static_cast<int>(g_lines.size());
	if (g_selection.anchor >= count || g_selection.current >= count) {
		g_selection = {};
	}
	const ImVec2 mouse   = ImGui::GetIO().MousePos;
	const bool   hovered = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
	// A faint highlight under the mouse, and a tooltip after a pause on a line, hint that lines
	// can be selected.
	static Clock::time_point hover_start {};
	const int                hovered_line = hovered ? LineAt(mouse) : -1;
	if (hovered_line != g_hovered_line) {
		hover_start = Clock::now();
	}
	g_hovered_line = hovered_line;
	if (hovered_line >= 0 && !g_selection.dragging && !LineSelected(hovered_line) &&
	    Clock::now() - hover_start >= std::chrono::seconds(1)) {
		ImGui::SetTooltip("Click a line to copy it, drag to copy several");
	}
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered) {
		const int line = LineAt(mouse);
		g_selection    = {line, line, line >= 0};
	}
	if (!g_selection.dragging) {
		return;
	}
	if (const int line = LineAt(mouse); line >= 0) {
		g_selection.current = line;
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		g_selection.dragging = false;
		CopySelection();
	}
}

} // namespace

void PerformanceOverlayRecordFlip(uint32_t guest_width, uint32_t guest_height) noexcept {
	if (!Common::DebugCounters::Enabled()) {
		return;
	}
	const auto       totals = ReadCounters();
	std::scoped_lock lock(g_frame_mutex);
	const auto       now = Clock::now();
	if (g_frames.window_start == Clock::time_point {}) {
		g_frames.window_start = now;
	}
	auto& frames = g_frames;
	if (frames.last_flip != Clock::time_point {}) {
		const float ms = std::chrono::duration<float, std::milli>(now - frames.last_flip).count();
		frames.intervals_ms[frames.next] = ms;
		frames.next                      = (frames.next + 1) % FRAME_HISTORY;
		frames.count                     = std::min(frames.count + 1, FRAME_HISTORY);
		frames.max_ms                    = std::max(frames.max_ms, ms);
		if (frames.recording && frames.recorded_ms.size() < MAX_RECORDED_FRAMES) {
			frames.recorded_ms.push_back(ms);
		}
		if (frames.average_ms > 0.0f &&
		    ms >= std::max(HITCH_MIN_MS, HITCH_FACTOR * frames.average_ms)) {
			Hitch hitch {now, ms, {}};
			for (size_t i = 0; i < COUNTER_COUNT; i++) {
				hitch.deltas[i] = totals[i] - frames.flip_totals[i];
			}
			if (frames.recording && frames.recorded_hitches.size() < MAX_RECORDED_HITCH) {
				frames.recorded_hitches.push_back(hitch);
			}
			if (frames.hitches.size() == HITCH_HISTORY) {
				frames.hitches.pop_front();
			}
			frames.hitches.push_back(hitch);
		}
		frames.average_ms = frames.average_ms > 0.0f ? frames.average_ms * 0.95f + ms * 0.05f : ms;
	}
	frames.flip_totals  = totals;
	frames.last_flip    = now;
	frames.guest_width  = guest_width;
	frames.guest_height = guest_height;
	frames.flips++;
}

bool PerformanceOverlayEnabled() noexcept {
	return Common::DebugCounters::Enabled();
}

void TogglePerformanceOverlay() noexcept {
	SetOverlayEnabled(!Common::DebugCounters::Enabled());
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

void TogglePerformanceOverlayDetails() noexcept {
	if (Common::DebugCounters::Enabled()) {
		g_details.store(!g_details.load(std::memory_order_acquire), std::memory_order_release);
	} else {
		g_details.store(true, std::memory_order_release);
		SetOverlayEnabled(true);
	}
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

void TogglePerformanceOverlayRecording() noexcept {
	if (!Common::DebugCounters::Enabled()) {
		SetOverlayEnabled(true);
	}
	g_record_toggle.store(true, std::memory_order_release);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t PerformanceOverlayRevision() noexcept {
	return g_revision.load(std::memory_order_acquire);
}

void RequestPerformanceOverlayRepaint() noexcept {
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

bool PerformanceOverlayContains(float x, float y) noexcept {
	if (!PerformanceOverlayEnabled()) {
		return false;
	}
	std::scoped_lock lock(g_panel_mutex);
	return x >= g_panel_min.x && x < g_panel_max.x && y >= g_panel_min.y && y < g_panel_max.y;
}

float DrawPerformanceOverlay(const GraphicContext& graphics, vk::Extent2D extent) {
	if (!PerformanceOverlayEnabled()) {
		return 0.0f;
	}
	EnsureSampler();

	static Snapshot shown;
	static uint64_t shown_generation = 0;
	static bool     shown_details    = false;
	const auto      generation       = g_generation.load(std::memory_order_acquire);
	const bool      details          = g_details.load(std::memory_order_acquire);
	const auto      now              = Clock::now();
	if (generation != shown_generation) {
		shown            = {};
		shown_generation = generation;
		g_recording      = {}; // Re-enabling the panel cleared the recorded frames.
	}
	static bool baseline_loaded = false;
	if (!std::exchange(baseline_loaded, true)) {
		LoadBaseline();
	}
	if (g_record_toggle.exchange(false, std::memory_order_acq_rel)) {
		if (g_recording.active) {
			StopRecording(now, true);
		} else {
			StartRecording(now);
		}
	}
	if (details != shown_details) {
		g_selection = {};
	}
	if (shown.sample_time == Clock::time_point {} || now - shown.sample_time >= REFRESH_INTERVAL ||
	    details != shown_details) {
		{
			std::scoped_lock lock(g_frame_mutex);
			auto&            frames = g_frames;
			shown.history_count     = frames.count;
			for (size_t i = 0; i < frames.count; i++) {
				shown.history[i] =
				    frames.intervals_ms[(frames.next + FRAME_HISTORY - frames.count + i) %
				                        FRAME_HISTORY];
			}
			if (frames.window_start == Clock::time_point {}) {
				frames.window_start = now;
			}
			const double seconds = std::chrono::duration<double>(now - frames.window_start).count();
			shown.fps            = seconds > 0.0 ? frames.flips / seconds : 0.0;
			shown.frame_ms       = frames.flips > 0 ? seconds * 1000.0 / frames.flips : 0.0;
			shown.max_frame_ms   = frames.max_ms;
			shown.stalled_s   = frames.last_flip == Clock::time_point {}
			                        ? 0.0
			                        : std::chrono::duration<double>(now - frames.last_flip).count();
			shown.guest_width = frames.guest_width;
			shown.guest_height  = frames.guest_height;
			shown.hitches       = frames.hitches;
			frames.flips        = 0;
			frames.max_ms       = 0.0f;
			frames.window_start = now;
		}
		shown_details = details;
		if (shown.history_count > 0) {
			auto       sorted     = shown.history;
			const auto end        = sorted.begin() + shown.history_count;
			const auto percentile = sorted.begin() + shown.history_count * 99 / 100;
			std::nth_element(sorted.begin(), percentile, end);
			shown.low_fps = *percentile > 0.0f ? 1000.0 / *percentile : 0.0;
		}
		Refresh(shown, graphics, now, generation);
		if (g_recording.active && g_recording.fields.empty()) {
			RecordSample(shown, graphics, extent, now);
			if (g_recording.samples.size() >= MAX_RECORDED_SAMPLES) {
				StopRecording(now, true);
			}
		}
		g_recording.fields_pending = g_recording.active && !g_recording.fields.empty();
		g_triggers_pending         = !g_triggers.empty();
	}

	const float scale = std::clamp(std::min(static_cast<float>(extent.width) / 1920.0f,
	                                        static_cast<float>(extent.height) / 1080.0f),
	                               0.75f, 1.5f);
	ImGui::SetNextWindowPos({static_cast<float>(extent.width) - 12.0f * scale, 12.0f * scale},
	                        ImGuiCond_Always, {1.0f, 0.0f});
	ImGui::SetNextWindowBgAlpha(details ? 0.82f : 0.72f);
	constexpr ImGuiWindowFlags FLAGS =
	    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
	    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
	    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_AlwaysAutoResize;
	ImGui::PushFont(nullptr, (details ? 13.0f : 15.0f) * scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f * scale, 8.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * scale, 2.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f * scale, 1.0f * scale});
	if (ImGui::Begin("##kyty_performance_overlay", nullptr, FLAGS)) {
		g_lines.clear();
		g_section.clear();
		if (shown.fps > 0.0) {
			const auto text = Fmt("%.1f FPS   %.1f ms", shown.fps, shown.frame_ms);
			ImGui::TextColored(Color(FrameTimeColor(shown.frame_ms)), "%s", text.c_str());
			AddItemLine(text, "FPS");
		} else if (shown.stalled_s >= 1.0) {
			const auto text = Fmt("No frames for %.0f s", shown.stalled_s);
			ImGui::TextColored(Color(BAD), "%s", text.c_str());
			AddItemLine(text);
		} else {
			ImGui::TextDisabled("Waiting for frames");
		}
		DrawFrameGraph(shown.history, shown.history_count, (details ? 330.0f : 260.0f) * scale,
		               46.0f * scale);
		if (details) {
			// Three columns keep the detailed panel within a 720p window.
			if (ImGui::BeginTable("##details", 3, TABLE_FLAGS)) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				DrawFramesAndCpu(shown, extent);
				ImGui::TableNextColumn();
				DrawGpu(shown, graphics);
				ImGui::TableNextColumn();
				DrawMemoryAndIo(shown);
				ImGui::EndTable();
			}
		} else {
			DrawCompact(shown);
		}
		if (g_recording.fields_pending) {
			g_recording.fields_pending = false;
			RecordFieldsSample(now);
			if (g_recording.samples.size() >= MAX_RECORDED_SAMPLES) {
				StopRecording(now, true);
			}
		}
		if (std::exchange(g_triggers_pending, false)) {
			CheckTriggers(shown, graphics, extent, now);
		}
		// The compact panel keeps only text; its tools live in the detailed one.
		if (!details) {
			if (g_recording.active) {
				ImGui::TextColored(Color(BAD), "Recording %.0f s, F3 stops and saves",
				                   RecordingSeconds(now));
			}
		} else {
			if (!g_triggers.empty()) {
				DrawTriggers(scale);
			}
			if (ImGui::SmallButton("Copy JSON")) {
				nlohmann::ordered_json json;
				ExportSnapshot(json, shown, graphics, extent, true);
				if (!g_triggers.empty() || !g_trigger_events.empty()) {
					json.update(TriggersJson(now));
				}
				SetHostClipboardText(Dump(json));
				SetStatus("Copied JSON");
			}
			auto selected_fields = SelectedFields();
			ImGui::SameLine();
			// "###" keeps a button's id while its label counts up.
			if (g_recording.active) {
				const double elapsed = RecordingSeconds(now);
				const auto   label   = g_recording.fields.empty()
				                           ? Fmt("Stop and copy stats (%.0f s)###record", elapsed)
				                           : Fmt("Stop and copy %s (%.0f s)###record",
				                                 FieldsName(g_recording.fields).c_str(), elapsed);
				if (ImGui::SmallButton(label.c_str())) {
					StopRecording(now, false);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Stop and save###record_save")) {
					StopRecording(now, true);
				}
				ImGui::SetItemTooltip(
				    "Saves the recording to a new file in perf_recordings next to "
				    "the emulator");
			} else {
				if (ImGui::SmallButton("Record stats###record")) {
					StartRecording(now);
				}
				ImGui::SetItemTooltip("Records every frame time, hitches and a snapshot of all "
				                      "overlay stats each second until stopped (F3 also starts it, "
				                      "and stops and saves it)");
				if (g_last_recording) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Set as baseline")) {
						SetBaseline();
					}
					ImGui::SetItemTooltip(
					    "Saves the last recording as this game's baseline in "
					    "perf_recordings; the overlay then compares live stats with "
					    "it, also after a restart");
				}
				if (!selected_fields.empty()) {
					ImGui::SameLine();
					if (ImGui::SmallButton(
					        Fmt("Record %s###record_fields", FieldsName(selected_fields).c_str())
					            .c_str())) {
						StartRecording(now, selected_fields);
					}
					ImGui::SetItemTooltip("Records the selected lines' values each second until "
					                      "stopped");
				}
			}
			if (selected_fields.size() == 1 && g_triggers.size() < MAX_TRIGGERS &&
			    std::none_of(g_triggers.begin(), g_triggers.end(), [&](const Trigger& trigger) {
				    return trigger.field == selected_fields.front();
			    })) {
				ImGui::SameLine();
				if (ImGui::SmallButton(
				        Fmt("Trigger on %s###trigger", selected_fields.front().c_str()).c_str())) {
					AddTrigger(selected_fields.front());
				}
				ImGui::SetItemTooltip("Saves a snapshot of all stats whenever this value crosses a "
				                      "threshold you set below");
			}
		}
		if (!g_status.empty() && now - g_status_time < STATUS_DURATION) {
			ImGui::TextColored(Color(GOOD), "%s", g_status.c_str());
		} else {
#if defined(__APPLE__)
			ImGui::TextDisabled(details ? "Cmd+Shift+P compact   Cmd+P hide   F3 record"
			                            : "Cmd+Shift+P details   Cmd+P hide   F3 record");
#else
			ImGui::TextDisabled(details ? "Shift+F2 compact   F2 hide   F3 record"
			                            : "Shift+F2 details   F2 hide   F3 record");
#endif
		}
		UpdateSelection();
	}
	const ImVec2 position = ImGui::GetWindowPos();
	const ImVec2 size     = ImGui::GetWindowSize();
	{
		std::scoped_lock lock(g_panel_mutex);
		g_panel_min = position;
		g_panel_max = {position.x + size.x, position.y + size.y};
	}
	const float bottom = position.y + size.y;
	ImGui::End();
	ImGui::PopStyleVar(4);
	ImGui::PopFont();
	return bottom;
}

} // namespace Libs::Graphics
