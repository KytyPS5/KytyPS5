#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/renderer/upscaler/fsr.h"

#include <api/include/vk/ffx_api_vk.h>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <tuple>
#include <upscalers/include/ffx_upscale.h>

namespace {

using namespace Libs::Graphics;

VkImage Image(uintptr_t handle) {
	return reinterpret_cast<VkImage>(handle);
}

Pssr::Request Request(uint32_t width, uint32_t height) {
	Pssr::Request request {};
	request.width  = width;
	request.height = height;
	request.flags  = Pssr::kReset | Pssr::kReverseDepth;
	return request;
}

TEST(FsrTest, ContextsTakeRenderSizesUpToTheColorInput) {
	Dlss::Inputs inputs {};
	inputs.color  = {.width = 2560, .height = 1440};
	inputs.output = {.width = 3840, .height = 2160};
	using Creation = std::optional<Dlss::Creation>;
	EXPECT_EQ(Fsr::Choose(Request(1920, 1080), inputs),
	          Creation({3840, 2160, Pssr::kReverseDepth, 0, 1, 1, 2560, 1440}));
	EXPECT_EQ(Fsr::Choose(Request(2561, 1080), inputs), std::nullopt);
	// FSR doesn't downscale.
	inputs.color = {.width = 4096, .height = 2304};
	EXPECT_EQ(Fsr::Choose(Request(3840, 2160), inputs),
	          Creation({3840, 2160, Pssr::kReverseDepth, 0, 1, 1, 3840, 2160}));
	EXPECT_EQ(Fsr::Choose(Request(4096, 2304), inputs), std::nullopt);
}

TEST(FsrTest, ReadsRawImagesInKnownFormatsFromTheOrigin) {
	Dlss::Inputs inputs {};
	inputs.color  = {.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32};
	inputs.depth  = {.format = VK_FORMAT_D32_SFLOAT};
	inputs.motion = {.format = VK_FORMAT_R16G16_SFLOAT};
	inputs.output = {.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32};
	auto request  = Request(1920, 1080);
	EXPECT_TRUE(Fsr::Reads(request, inputs));
	request.x = 8;
	EXPECT_FALSE(Fsr::Reads(request, inputs));
	request.x            = 0;
	inputs.motion.format = VK_FORMAT_R16G16_UNORM;
	EXPECT_FALSE(Fsr::Reads(request, inputs));
}

TEST(FsrTest, CreationSetsTheFlagsAndSizes) {
	ffxCreateContextDescUpscale desc {};
	Fsr::SetCreation(desc, {3840, 2160, Pssr::kReverseDepth | Pssr::kAutoExposure, 0, 1, 1, 2560,
	                        1440});
	EXPECT_EQ(desc.flags,
	          static_cast<uint32_t>(FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
	                                FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
	                                FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
	                                FFX_UPSCALE_ENABLE_AUTO_EXPOSURE));
	EXPECT_EQ(std::tie(desc.maxRenderSize.width, desc.maxRenderSize.height,
	                   desc.maxUpscaleSize.width, desc.maxUpscaleSize.height),
	          std::tuple(2560u, 1440u, 3840u, 2160u));
	Fsr::SetCreation(desc, {3840, 2160, 0, 0, 1, 1, 2560, 1440});
	EXPECT_EQ(desc.flags, static_cast<uint32_t>(FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
	                                            FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION));
}

TEST(FsrTest, DispatchPassesTheRequest) {
	auto request            = Request(1600, 900);
	request.jitter[0]       = 0.25f;
	request.jitter[1]       = -0.375f;
	request.motion_scale[0] = -800.0f;
	request.motion_scale[1] = 450.0f;
	Dlss::Inputs inputs {};
	inputs.color  = {.image = Image(1), .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .width = 1920,
	                 .height = 1080};
	inputs.depth  = {.image = Image(2), .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .width = 1920,
	                 .height = 1080};
	inputs.motion = {.image = Image(3), .format = VK_FORMAT_R16G16_SFLOAT, .width = 1920,
	                 .height = 1080};
	inputs.output = {.image = Image(4), .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
	                 .width = 3840, .height = 2160, .read_write = 1};
	ffxDispatchDescUpscale desc {};
	Fsr::SetDispatch(desc, request, inputs, true, 16.0f);
	EXPECT_EQ(desc.color.resource, Image(1));
	EXPECT_EQ(desc.color.description.format, FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT);
	EXPECT_EQ(desc.color.description.usage, FFX_API_RESOURCE_USAGE_READ_ONLY);
	EXPECT_EQ(desc.depth.description.usage,
	          FFX_API_RESOURCE_USAGE_DEPTHTARGET | FFX_API_RESOURCE_USAGE_STENCILTARGET);
	EXPECT_EQ(desc.motionVectors.description.format, FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
	EXPECT_EQ(desc.exposure.resource, nullptr);
	EXPECT_EQ(desc.output.description.usage, FFX_API_RESOURCE_USAGE_UAV);
	EXPECT_EQ(std::tie(desc.jitterOffset.x, desc.jitterOffset.y), std::tuple(0.25f, -0.375f));
	EXPECT_EQ(std::tie(desc.motionVectorScale.x, desc.motionVectorScale.y),
	          std::tuple(-800.0f, 450.0f));
	EXPECT_EQ(std::tie(desc.renderSize.width, desc.renderSize.height), std::tuple(1600u, 900u));
	EXPECT_EQ(std::tie(desc.upscaleSize.width, desc.upscaleSize.height),
	          std::tuple(3840u, 2160u));
	EXPECT_EQ(desc.frameTimeDelta, 16.0f);
	EXPECT_EQ(desc.preExposure, 1.0f); // defaults to 1
	EXPECT_TRUE(desc.reset);

	Dlss::Resource exposure {.image = Image(5), .format = VK_FORMAT_R32_SFLOAT, .width = 1,
	                         .height = 1};
	inputs.exposure      = &exposure;
	request.pre_exposure = 0.5f;
	Fsr::SetDispatch(desc, request, inputs, false, 16.0f);
	EXPECT_EQ(desc.exposure.resource, Image(5));
	EXPECT_EQ(desc.preExposure, 0.5f);
	EXPECT_FALSE(desc.reset);
}

TEST(FsrTest, LoadsOnlyWhenSelectedWithTheLoader) {
	Config::Initialize();
	using enum Config::PssrUpscaler;
	for (const auto& [pro, upscaler, folder]:
	     {std::tuple {false, FSR, "fsr"}, {true, DLSS, "fsr"}, {true, FSR, ""},
	      {true, FSR, "missing"}}) {
		Config::ConfigOptions options;
		options.pro_enabled   = pro;
		options.pssr_upscaler = upscaler;
		options.fsr_folder    = folder;
		Config::Load(options);
		Fsr::Load();
		EXPECT_FALSE(Fsr::Enabled()) << pro << ' ' << static_cast<int>(upscaler) << ' ' << folder;
		Fsr::Shutdown();
	}
	Config::Shutdown();
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
