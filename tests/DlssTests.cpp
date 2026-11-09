#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/renderer/dlss.h"

#include <SDL3/SDL_loadso.h>
#include <array>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace {

using namespace Libs::Graphics;

using Value = std::variant<unsigned long long, float, double, unsigned int, int, void*>;

constexpr int MISSING = static_cast<int>(0xbad00000u);

class Recorder final: public Dlss::Parameter {
public:
	std::map<std::string, Value> values;

	void Set(const char* name, unsigned long long value) override { values[name] = value; }
	void Set(const char* name, float value) override { values[name] = value; }
	void Set(const char* name, double value) override { values[name] = value; }
	void Set(const char* name, unsigned int value) override { values[name] = value; }
	void Set(const char* name, int value) override { values[name] = value; }
	void Set(const char* /*name*/, Dlss::D3D11Resource* /*value*/) override { ADD_FAILURE(); }
	void Set(const char* /*name*/, Dlss::D3D12Resource* /*value*/) override { ADD_FAILURE(); }
	void Set(const char* name, void* value) override { values[name] = value; }
	int  Get(const char* name, unsigned long long* value) const override {
		return Read(name, value);
	}
	int Get(const char* name, float* value) const override { return Read(name, value); }
	int Get(const char* name, double* value) const override { return Read(name, value); }
	int Get(const char* name, unsigned int* value) const override { return Read(name, value); }
	int Get(const char* name, int* value) const override { return Read(name, value); }
	int Get(const char* /*name*/, Dlss::D3D11Resource** /*value*/) const override {
		return MISSING;
	}
	int Get(const char* /*name*/, Dlss::D3D12Resource** /*value*/) const override {
		return MISSING;
	}
	int  Get(const char* name, void** value) const override { return Read(name, value); }
	void Reset() override {}

private:
	template <typename T>
	int Read(const char* name, T* value) const {
		const auto found = values.find(name);
		if (found == values.end() || !std::holds_alternative<T>(found->second)) {
			return MISSING;
		}
		*value = std::get<T>(found->second);
		return 1;
	}
};

// Optimal, smallest and largest render widths of each quality mode at a 3840x2160 output, with
// 16:9 heights; an optimal width of zero is a mode the model lacks.
struct ModeSizes {
	uint32_t optimal;
	uint32_t min;
	uint32_t max;
};
constexpr std::array<ModeSizes, 6> MODES {{{1920, 1280, 2560},
                                           {2227, 1484, 2560},
                                           {2560, 1707, 2560},
                                           {1280, 1280, 1280},
                                           {0, 1280, 3840},
                                           {3840, 1280, 3840}}};
bool g_dynamic = true; // whether the model reports dynamic render ranges

int OptimalSettings(Dlss::Parameter* parameters) {
	int      mode  = -1;
	uint32_t width = 0;
	if (parameters->Get("PerfQualityValue", &mode) != 1 || parameters->Get("Width", &width) != 1 ||
	    width != 3840) {
		return MISSING;
	}
	const auto& sizes = MODES[mode];
	parameters->Set("OutWidth", sizes.optimal);
	parameters->Set("OutHeight", sizes.optimal * 9 / 16);
	if (g_dynamic) {
		parameters->Set("DLSS.Get.Dynamic.Min.Render.Width", sizes.min);
		parameters->Set("DLSS.Get.Dynamic.Min.Render.Height", sizes.min * 9 / 16);
		parameters->Set("DLSS.Get.Dynamic.Max.Render.Width", sizes.max);
		parameters->Set("DLSS.Get.Dynamic.Max.Render.Height", sizes.max * 9 / 16);
	}
	return 1;
}

// An object laid out like the driver's: a table of methods that note their slot.
struct Slots {
	const void* const* table;
	int                slot = -1;
};

template <int N>
int Note(Slots* self) {
	self->slot = N;
	return 1;
}

template <int... N>
constexpr std::array<const void*, sizeof...(N)> MakeTable(std::integer_sequence<int, N...>) {
	return {reinterpret_cast<const void*>(&Note<N>)...};
}

const auto TABLE = MakeTable(std::make_integer_sequence<int, 17> {});

Pssr::Request TemporalRequest() {
	Pssr::Request request {};
	request.depth[0]  = 0x100;
	request.motion[0] = 0x200;
	return request;
}

TEST(DlssTest, ParameterSlotsFollowTheDriverInterface) {
	Slots      raw {TABLE.data()};
	auto&      parameter = *reinterpret_cast<Dlss::Parameter*>(&raw);
	const auto slot      = [&](auto call) {
		raw.slot = -1;
		call();
		return raw.slot;
	};
	// The Itanium ABI keeps the declaration order; MSVC reverses each overload group.
#if defined(_WIN32)
	const std::array<int, 17> expected {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8, 16};
#else
	const std::array<int, 17> expected {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
#endif
	unsigned long long        u64     = 0;
	float                     f32     = 0;
	double                    f64     = 0;
	unsigned int              u32     = 0;
	int                       i32     = 0;
	Dlss::D3D11Resource*      d3d11   = nullptr;
	Dlss::D3D12Resource*      d3d12   = nullptr;
	void*                     pointer = nullptr;
	const std::array<int, 17> slots {slot([&] { parameter.Set("", u64); }),
	                                 slot([&] { parameter.Set("", f32); }),
	                                 slot([&] { parameter.Set("", f64); }),
	                                 slot([&] { parameter.Set("", u32); }),
	                                 slot([&] { parameter.Set("", i32); }),
	                                 slot([&] { parameter.Set("", d3d11); }),
	                                 slot([&] { parameter.Set("", d3d12); }),
	                                 slot([&] { parameter.Set("", pointer); }),
	                                 slot([&] { (void)parameter.Get("", &u64); }),
	                                 slot([&] { (void)parameter.Get("", &f32); }),
	                                 slot([&] { (void)parameter.Get("", &f64); }),
	                                 slot([&] { (void)parameter.Get("", &u32); }),
	                                 slot([&] { (void)parameter.Get("", &i32); }),
	                                 slot([&] { (void)parameter.Get("", &d3d11); }),
	                                 slot([&] { (void)parameter.Get("", &d3d12); }),
	                                 slot([&] { (void)parameter.Get("", &pointer); }),
	                                 slot([&] { parameter.Reset(); })};
	EXPECT_EQ(slots, expected);
}

TEST(DlssTest, TemporalRequestsNeedDepthAndMotion) {
	auto request = TemporalRequest();
	EXPECT_TRUE(Dlss::Accepts(request));
	request.flags = Pssr::kSpatialOnly;
	EXPECT_FALSE(Dlss::Accepts(request));
	request          = TemporalRequest();
	request.depth[0] = 0;
	EXPECT_FALSE(Dlss::Accepts(request));
	request           = TemporalRequest();
	request.motion[0] = 0;
	EXPECT_FALSE(Dlss::Accepts(request));
}

TEST(DlssTest, ModeRangesPickTheCreation) {
	Recorder capabilities;
	capabilities.values["DLSSOptimalSettingsCallback"] = reinterpret_cast<void*>(&OptimalSettings);
	Dlss::Inputs inputs {};
	inputs.output     = {.width = 3840, .height = 2160};
	const auto choose = [&](uint32_t width, uint32_t height) {
		auto request   = TemporalRequest();
		request.width  = width;
		request.height = height;
		request.flags  = Pssr::kReset | Pssr::kReverseDepth;
		return Dlss::Choose(capabilities, request, inputs);
	};
	using Creation = std::optional<Dlss::Creation>;
	// Of the modes taking a size, the one with the nearest optimal size wins.
	EXPECT_EQ(choose(1920, 1080),
	          Creation({3840, 2160, Pssr::kReverseDepth, 0, 1280, 720, 2560, 1440}));
	EXPECT_EQ(choose(1280, 720),
	          Creation({3840, 2160, Pssr::kReverseDepth, 3, 1280, 720, 1280, 720}));
	// DLAA takes the output size only, and an absent mode takes nothing.
	EXPECT_EQ(choose(3840, 2160),
	          Creation({3840, 2160, Pssr::kReverseDepth, 5, 3840, 2160, 3840, 2160}));
	EXPECT_EQ(choose(3000, 1688), std::nullopt);

	// Without dynamic ranges each mode takes its optimal size.
	g_dynamic = false;
	capabilities.values.clear();
	capabilities.values["DLSSOptimalSettingsCallback"] = reinterpret_cast<void*>(&OptimalSettings);
	EXPECT_EQ(choose(2227, 1252),
	          Creation({3840, 2160, Pssr::kReverseDepth, 1, 2227, 1252, 2227, 1252}));
	EXPECT_EQ(choose(2000, 1125), std::nullopt);
	g_dynamic = true;
}

TEST(DlssTest, FeaturesTakeRequestsInTheirRange) {
	const Dlss::Creation creation {3840, 2160, Pssr::kReverseDepth, 0, 1280, 720, 2560, 1440};
	Dlss::Inputs         inputs {};
	inputs.output    = {.width = 3840, .height = 2160};
	auto request     = TemporalRequest();
	request.width    = 1920;
	request.height   = 1080;
	request.flags    = Pssr::kReset | Pssr::kReverseDepth;
	const auto takes = [&](auto change) {
		auto changed_request = request;
		auto changed_inputs  = inputs;
		change(changed_request, changed_inputs);
		return Dlss::Takes(creation, changed_request, changed_inputs);
	};
	EXPECT_TRUE(takes([](auto&, auto&) {}));
	EXPECT_FALSE(takes([](auto&, auto& changed) { changed.output.width = 2560; }));
	EXPECT_FALSE(takes([](auto&, auto& changed) { changed.output.height = 1440; }));
	EXPECT_FALSE(takes([](auto& changed, auto&) { changed.flags |= Pssr::kAutoExposure; }));
	EXPECT_FALSE(takes([](auto& changed, auto&) { changed.width = 1279; }));
	EXPECT_FALSE(takes([](auto& changed, auto&) { changed.width = 2561; }));
	EXPECT_FALSE(takes([](auto& changed, auto&) { changed.height = 719; }));
	EXPECT_FALSE(takes([](auto& changed, auto&) { changed.height = 1441; }));
}

TEST(DlssTest, CreationSetsTheModeAndFlags) {
	Recorder recorder;
	Dlss::SetCreation(
	    recorder, {3840, 2160, Pssr::kReverseDepth | Pssr::kAutoExposure, 2, 1707, 960, 2560, 1440},
	    11);
	auto& values = recorder.values;
	EXPECT_EQ(values["CreationNodeMask"], Value(1u));
	EXPECT_EQ(values["VisibilityNodeMask"], Value(1u));
	EXPECT_EQ(values["Width"], Value(2560u));
	EXPECT_EQ(values["Height"], Value(1440u));
	EXPECT_EQ(values["OutWidth"], Value(3840u));
	EXPECT_EQ(values["OutHeight"], Value(2160u));
	EXPECT_EQ(values["PerfQualityValue"], Value(2));
	EXPECT_EQ(values["DLSS.Feature.Create.Flags"],
	          Value((1 << 0) | (1 << 1) | (1 << 3) | (1 << 6)));
	for (const char* mode:
	     {"DLAA", "Quality", "Balanced", "Performance", "UltraPerformance", "UltraQuality"}) {
		EXPECT_EQ(values[std::string("DLSS.Hint.Render.Preset.") + mode], Value(11u)) << mode;
	}
	EXPECT_EQ(values.size(), 14u);

	Dlss::SetCreation(recorder, {3840, 2160, 0, 2, 1707, 960, 2560, 1440}, 0);
	EXPECT_EQ(values["DLSS.Feature.Create.Flags"], Value((1 << 0) | (1 << 1)));
}

TEST(DlssTest, EvaluationPassesTheRequest) {
	auto request            = TemporalRequest();
	request.width           = 1600;
	request.height          = 900;
	request.x               = 8;
	request.y               = 4;
	request.jitter[0]       = 0.25f;
	request.jitter[1]       = -0.375f;
	request.motion_scale[0] = -800.0f;
	request.motion_scale[1] = 450.0f;
	request.pre_exposure    = 0.5f;
	Dlss::Resource exposure {};
	Dlss::Inputs   inputs {};
	Recorder       recorder;
	Dlss::SetEvaluation(recorder, request, inputs, true);
	auto& values = recorder.values;
	EXPECT_EQ(values["Color"], Value(static_cast<void*>(&inputs.color)));
	EXPECT_EQ(values["Depth"], Value(static_cast<void*>(&inputs.depth)));
	EXPECT_EQ(values["MotionVectors"], Value(static_cast<void*>(&inputs.motion)));
	EXPECT_EQ(values["Output"], Value(static_cast<void*>(&inputs.output)));
	EXPECT_EQ(values["ExposureTexture"], Value(static_cast<void*>(nullptr)));
	EXPECT_EQ(values["Jitter.Offset.X"], Value(0.25f));
	EXPECT_EQ(values["Jitter.Offset.Y"], Value(-0.375f));
	EXPECT_EQ(values["MV.Scale.X"], Value(-800.0f));
	EXPECT_EQ(values["MV.Scale.Y"], Value(450.0f));
	EXPECT_EQ(values["DLSS.Pre.Exposure"], Value(0.5f));
	EXPECT_EQ(values["Reset"], Value(1));
	EXPECT_EQ(values["DLSS.Render.Subrect.Dimensions.Width"], Value(1600u));
	EXPECT_EQ(values["DLSS.Render.Subrect.Dimensions.Height"], Value(900u));
	for (const char* input: {"Color", "Depth", "MV"}) {
		const std::string base = std::string("DLSS.Input.") + input + ".Subrect.Base.";
		EXPECT_EQ(values[base + "X"], Value(8u)) << input;
		EXPECT_EQ(values[base + "Y"], Value(4u)) << input;
	}
	EXPECT_EQ(values.size(), 19u);

	inputs.exposure = &exposure;
	Dlss::SetEvaluation(recorder, request, inputs, false);
	EXPECT_EQ(values["ExposureTexture"], Value(static_cast<void*>(&exposure)));
	EXPECT_EQ(values["Reset"], Value(0));
}

// DLSS loads the driver's NGX library, which needs one instance extension, only in Pro mode with
// a model folder.
TEST(DlssTest, LoadsOnlyInProModeWithAFolder) {
	auto* ngx = SDL_LoadObject("libnvidia-ngx.so.1");
	if (ngx == nullptr) {
		GTEST_SKIP() << "NVIDIA's Linux driver is not installed";
	}
	SDL_UnloadObject(ngx);
	vk::ExtensionProperties properties2;
	properties2.extensionName = std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	Config::Initialize();
	for (const auto& [pro, folder]: {std::pair {false, "dlss"}, {true, ""}, {true, "dlss"}}) {
		Config::ConfigOptions options;
		options.pro_enabled = pro;
		options.dlss_folder = folder;
		Config::Load(options);
		Dlss::Load();
		std::vector<const char*> extensions;
		Dlss::AppendExtensions(extensions, {properties2}, false);
		EXPECT_EQ(extensions.size(), pro && *folder != '\0' ? 1u : 0u) << pro << folder;
		Dlss::Shutdown();
	}
	Config::Shutdown();
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
