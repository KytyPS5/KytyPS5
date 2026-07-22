#include "graphics/regression/frameRegressionVulkan.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/regression/frameRegression.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <cstdio>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Libs::Graphics::Regression {
namespace {

constexpr int FailureExitCode = 2;

class Controller {
public:
	bool Initialize() {
		const auto configured_mode = Config::GetFrameRegressionMode();
		if (configured_mode == Config::FrameRegressionMode::None) {
			return true;
		}
		enabled            = true;
		exit_on_complete   = Config::FrameRegressionExitOnComplete();
		const auto& properties = GetRenderContext().GetGraphics().GetPhysicalDeviceProperties();
		Options options {
		    .mode = configured_mode == Config::FrameRegressionMode::Record ? Mode::Record
		                                                                 : Mode::Compare,
		    .baseline = Config::GetFrameRegressionBaseline(),
		    .report = Config::GetFrameRegressionReport(),
		    .capture = Config::GetGpuCaptureFile(),
		    .frame_ordinals = Config::GetFrameRegressionFrames(),
		    .provenance = {
		        .test_id = Config::GetFrameRegressionTestId(),
		        .build_id = KYTY_GIT_VERSION,
		        .gpu_name = properties.deviceName.data(),
		        .configuration = fmt::format(
		            "screen={}x{};vblank={};vulkan_validation={};shader_validation={};"
		            "shader_optimization={};spirv_debug_printf={};ngg_rectlist={}",
		            Config::GetScreenWidth(), Config::GetScreenHeight(), Config::GetVblankFrequency(),
		            Config::VulkanValidationEnabled(), Config::ShaderValidationEnabled(),
		            static_cast<uint32_t>(Config::GetShaderOptimizationType()),
		            Config::SpirvDebugPrintfEnabled(), Config::NggRectlistDrawEnabled()),
		        .gpu_vendor_id = properties.vendorID,
		        .gpu_device_id = properties.deviceID,
		        .gpu_driver = properties.driverVersion,
		        .vulkan_api = properties.apiVersion,
		    },
		    .save_raw_frames = Config::FrameRegressionSaveRaw(),
		    .allow_environment_mismatch =
		        Config::FrameRegressionAllowEnvironmentMismatch(),
		};
		session = Session::Create(std::move(options), error);
		if (session == nullptr) {
			Fail("initialization", error);
			return false;
		}
		LOGF("Frame regression enabled for %zu presentation ordinal(s)\n",
		     Config::GetFrameRegressionFrames().size());
		return true;
	}

	bool Observe(VulkanImage& image) {
		const auto ordinal = next_ordinal++;
		if (!enabled || session == nullptr || !session->WantsFrame(ordinal)) {
			return failed;
		}

		const auto format = ToPixelFormat(image.format);
		if (!format.has_value()) {
			Fail("readback", "unsupported presented-frame Vulkan format");
			return true;
		}
		const auto bytes_per_pixel = BytesPerPixel(*format);
		if (image.extent.width == 0 || image.extent.height == 0 || bytes_per_pixel == 0 ||
		    image.extent.width > std::numeric_limits<uint32_t>::max() / bytes_per_pixel) {
			Fail("readback", "invalid presented-frame dimensions");
			return true;
		}
		const auto row_pitch = image.extent.width * bytes_per_pixel;
		const auto byte_size = static_cast<uint64_t>(row_pitch) * image.extent.height;
		if (byte_size > std::numeric_limits<size_t>::max() || byte_size > MaxFrameBytes) {
			Fail("readback", "presented frame is too large for host memory");
			return true;
		}

		std::vector<uint8_t> bytes(static_cast<size_t>(byte_size));
		// Vulkan bufferRowLength is measured in texels; FrameView::row_pitch is measured in bytes.
		Transfer::DownloadImage(bytes.data(), byte_size, image.extent.width, image,
		                        vk::ImageLayout::eTransferSrcOptimal);
		FrameView frame {.ordinal = ordinal,
		                 .width = image.extent.width,
		                 .height = image.extent.height,
		                 .row_pitch = row_pitch,
		                 .format = *format,
		                 .bytes = bytes};
		error.clear();
		if (!session->Observe(frame, error)) {
			Fail("observation", error);
			return true;
		}
		if (session->Complete()) {
			LOGF("Frame regression completed with exit code %d\n", session->ExitCode());
			return exit_on_complete;
		}
		return false;
	}

	int Finalize() {
		if (!enabled) {
			return 0;
		}
		if (session == nullptr) {
			return FailureExitCode;
		}
		error.clear();
		const auto result = session->Finalize(error);
		if (!error.empty()) {
			Fail("finalization", error);
		}
		return failed ? FailureExitCode : result;
	}

private:
	static std::optional<PixelFormat> ToPixelFormat(vk::Format format) {
		switch (format) {
			case vk::Format::eR8G8B8A8Unorm: return PixelFormat::Rgba8Unorm;
			case vk::Format::eR8G8B8A8Srgb: return PixelFormat::Rgba8Srgb;
			case vk::Format::eB8G8R8A8Unorm: return PixelFormat::Bgra8Unorm;
			case vk::Format::eB8G8R8A8Srgb: return PixelFormat::Bgra8Srgb;
			case vk::Format::eA2B10G10R10UnormPack32: return PixelFormat::A2b10g10r10Unorm;
			case vk::Format::eA2R10G10B10UnormPack32: return PixelFormat::A2r10g10b10Unorm;
			case vk::Format::eR16G16B16A16Sfloat: return PixelFormat::Rgba16Float;
			default: return {};
		}
	}

	void Fail(const char* stage, const std::string& message) {
		failed = true;
		::printf("Frame regression %s failed: %s\n", stage, message.c_str());
		LOGF("Frame regression %s failed: %s\n", stage, message.c_str());
	}

	std::unique_ptr<Session> session;
	std::string              error;
	uint64_t                 next_ordinal     = 0;
	bool                     enabled          = false;
	bool                     failed           = false;
	bool                     exit_on_complete = true;
};

std::unique_ptr<Controller> g_controller;

} // namespace

bool InitializeFrameRegression() {
	if (g_controller != nullptr) {
		return false;
	}
	g_controller = std::make_unique<Controller>();
	return g_controller->Initialize();
}

bool ObservePresentedFrame(VulkanImage& image) {
	return g_controller != nullptr && g_controller->Observe(image);
}

int FinalizeFrameRegression() {
	return g_controller == nullptr ? 0 : g_controller->Finalize();
}

} // namespace Libs::Graphics::Regression
