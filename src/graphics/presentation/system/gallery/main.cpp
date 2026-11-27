// The System UI gallery as a program. It shows the design system's pages in a window of its own,
// so they can be reviewed without running a game. It shows one page at a time, picked from the
// sidebar, from the foundations, motion and theme to the primitives and components, with live
// toasts only on the Toast page.
//
// SDL3/SDL_main.h is deliberately not included. The emulator doesn't use it either, so main is a
// plain main on every platform and SDL doesn't rename it.

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/encodedImage.h"
#include "graphics/presentation/system/core/fonts.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/gallery/components/nav.h"
#include "graphics/presentation/system/gallery/pages/animationsPage.h"
#include "graphics/presentation/system/gallery/pages/boxPage.h"
#include "graphics/presentation/system/gallery/pages/cardPage.h"
#include "graphics/presentation/system/gallery/pages/colourPage.h"
#include "graphics/presentation/system/gallery/pages/elevationsPage.h"
#include "graphics/presentation/system/gallery/pages/glyphPage.h"
#include "graphics/presentation/system/gallery/pages/imagePage.h"
#include "graphics/presentation/system/gallery/pages/spacingPage.h"
#include "graphics/presentation/system/gallery/pages/speedPage.h"
#include "graphics/presentation/system/gallery/pages/themePage.h"
#include "graphics/presentation/system/gallery/pages/timingPage.h"
#include "graphics/presentation/system/gallery/pages/toastPage.h"
#include "graphics/presentation/system/gallery/pages/typographyPage.h"
#include "graphics/presentation/system/gallery/pointer.h"
#include "graphics/presentation/system/gallery/vulkanHost.h"
#include "graphics/presentation/system/imgui/imageCache.h"
#include "graphics/presentation/system/imgui/imageStore.h"
#include "graphics/presentation/system/instance.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace {

using namespace Libs::Graphics::SystemUi;
using namespace Libs::Graphics::SystemUi::Gallery;

constexpr const char* WINDOW_TITLE  = "KytyPS5 :: Design Library";
constexpr int         WINDOW_WIDTH  = 1920;
constexpr int         WINDOW_HEIGHT = 1080;
// How long to sleep while the window is minimised, so it doesn't spin a core.
constexpr uint32_t IDLE_DELAY_MS = 10;
constexpr float    MAX_CHANNEL   = 255.0f;
constexpr float    OPAQUE        = 1.0f;

// The background behind the pages, #0E1117, the darkest neutral that isn't near black. Surfaces
// drawn over it later read as lifted off it, as they do over a dark game, where pure black would
// make them look flat. The channels stay sRGB-encoded, as the swapchain is UNORM.
constexpr ImVec4 BACKGROUND {0x0E / MAX_CHANNEL, 0x11 / MAX_CHANNEL, 0x17 / MAX_CHANNEL, OPAQUE};

// Handles pending window events. Returns false once the window was asked to close.
bool PollEvents(SDL_Window* window) {
	bool      running = true;
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		ImGui_ImplSDL3_ProcessEvent(&event);
		if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
		                                     event.window.windowID == SDL_GetWindowID(window))) {
			running = false;
		}
	}
	return running;
}

// The image the image page shows, relative to the assets folder.
constexpr const char* ICON_FILE = "images/hidden-trophy.png";
// The largest side the gallery accepts for an image it loads.
constexpr int MAX_IMAGE_SIDE = 1024;

// What the gallery loads once and shows every frame.
struct GalleryAssets {
	FontsContext   fonts;
	ImTextureData* icon = nullptr;
	Vec2           icon_size;
	// The icon still encoded, for the trophies, which show it through the image cache.
	EncodedImage icon_png;
};

// The assets folder beside the executable, where the build copies the fonts and images as it does
// for the emulator.
std::string AssetsFolder() {
	const char* base_path = SDL_GetBasePath();
	if (base_path == nullptr) {
		// Assets are then looked for in the working directory instead, which may still work.
		std::fprintf(stderr, "system_ui_gallery: no executable folder (%s)\n", SDL_GetError());
	}
	return std::string(base_path != nullptr ? base_path : "") + "assets/";
}

// Loads the image page's icon from `folder` into `images` and gives it to `assets` with its size.
// When it can't be read, `assets` keeps no icon, which leaves the image page's slots empty.
void LoadGalleryIcon(ImageStore& images, const std::string& folder, GalleryAssets& assets) {
	std::ifstream file(folder + ICON_FILE, std::ios::binary);
	if (!file) {
		// Said here, as an empty read would otherwise reach the store as a vague decode failure.
		std::fprintf(stderr, "system_ui_gallery: could not open %s%s\n", folder.c_str(), ICON_FILE);
		return;
	}
	const std::string data((std::istreambuf_iterator<char>(file)),
	                       std::istreambuf_iterator<char>());
	const auto        id = images.Add(std::as_bytes(std::span(data)), MAX_IMAGE_SIDE);
	if (!id) {
		std::fprintf(stderr, "system_ui_gallery: could not load %s%s\n", folder.c_str(), ICON_FILE);
		return;
	}
	assets.icon      = images.Texture(*id);
	assets.icon_size = images.Size(*id);
	const auto bytes = std::as_bytes(std::span(data));
	assets.icon_png  = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
}

// The gallery page `page`, built in the current frame.
AnyElement BuildPage(GalleryPage page, const GalleryAssets& assets) {
	switch (page) {
		case GalleryPage::Typography: return BuildTypographyPage();
		case GalleryPage::Elevations: return BuildElevationsPage();
		case GalleryPage::Spacing: return BuildSpacingPage();
		case GalleryPage::Animations: return BuildAnimationsPage();
		case GalleryPage::Speed: return BuildSpeedPage();
		case GalleryPage::Timing: return BuildTimingPage();
		case GalleryPage::Theme: return BuildThemePage();
		case GalleryPage::Box: return BuildBoxPage();
		case GalleryPage::Image:
			return BuildImagePage({.texture = assets.icon, .size = assets.icon_size});
		case GalleryPage::Card: return BuildCardPage();
		case GalleryPage::Toast: return BuildToastPage({.icon = assets.icon_png});
		case GalleryPage::Glyphs: return BuildGlyphPage();
		case GalleryPage::Colours: break;
	}
	return BuildColourPage();
}

// Wraps `page` in a pane that scrolls with the wheel when the page is taller than the room beside
// the sidebar. Its key names the page, so a page shown anew starts at its top rather than at the
// offset the last page was scrolled to.
AnyElement PagePane(AnyElement page, GalleryPage shown) {
	// Stretch makes the page as wide as the pane, so its surface reaches the window's right edge
	// however narrow its content.
	return Box({.layout = {.direction = FlexDirection::Column, .align = Align::Stretch},
	            .self   = {.layout = {.grow = 1.0f},
	                       .styles = {.fill     = CurrentThemeContext().colors.surface.base,
	                                  .overflow = Overflow::Scroll}},
	            .key    = Key("page", static_cast<uint64_t>(shown))},
	           {page});
}

// Builds one frame of system UI with `theme` and `assets` and paints it under everything ImGui
// draws, sized to the display. The sidebar runs down the left edge, with the
// page, in a pane that scrolls, filling the room to its right. A click on last frame's sidebar
// first changes the page or reduced motion, so the frame shows the change. Sizes snap to whole
// ImGui units, which are whole pixels only while the framebuffer scale is one.
void DrawSystemUi(FrameBuilder& frame, const ThemeContext& theme, const GalleryAssets& assets,
                  NavState& nav) {
	const ImVec2       display = ImGui::GetIO().DisplaySize;
	const PointerInput pointer = PointerNow();
	UpdateNav(nav, pointer);
	frame.Begin();
	const ScaleContext scale(theme.scale, display.x, display.y);
	const FrameScope   scope(frame, theme, scale, assets.fonts, ClockContext::Now(),
	                         {.reduced = nav.reduced_motion}, WheelNow());
	const NavElements  sidebar = Nav({.state = nav, .pointer = pointer});
	const AnyElement   page    = PagePane(BuildPage(nav.page, assets), nav.page);

	// Stretch makes the sidebar and the pane as tall as the row, and the row grows to fill the
	// window. The sidebar sets its own width, and the pane grows into the rest.
	const AnyElement gallery_layout =
	    Box({.layout = {.align = Align::Stretch}, .self = {.layout = {.grow = 1.0f}}},
	        {sidebar.root, page});
	(void)Measure(gallery_layout, {display.x, display.y});
	// Arrange fails for a handle that isn't live or wasn't measured, and then nothing was placed.
	if (Arrange(gallery_layout, {0.0f, 0.0f, display.x, display.y})) {
		Paint(gallery_layout, *ImGui::GetBackgroundDrawList());
		RememberNav(nav, frame, sidebar);
		RememberElevationsHover(frame);
	}
}

// Runs frames until the window closes, each the system UI over the background. Returns a failure
// code when Vulkan can not continue.
int RunLoop(SDL_Window* window, VulkanHost& host, const ThemeContext& theme,
            const GalleryAssets& assets, ImageStore& images, Instance& system_ui) {
	ImageCache& image_cache = system_ui.Images();
	// Kept across frames, so its storage and each kind's frame hooks are reused instead of remade.
	FrameBuilder frame;
	// Kept across frames too, as its toasts carry on from one frame to the next.
	ToastDemo toast_demo(system_ui.Toasts(), assets.icon_png);
	// The page shown and reduced motion, kept across frames as the sidebar changes them.
	NavState nav;
	while (PollEvents(window)) {
		const FrameResult began = host.BeginFrame();
		if (began == FrameResult::Fatal) {
			return EXIT_FAILURE;
		}
		if (began == FrameResult::Skip) {
			SDL_Delay(IDLE_DELAY_MS);
			continue;
		}
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		// Between NewFrame and Render, as the store asks, so released images are forgotten safely.
		images.Collect();
		image_cache.BeginFrame();
		{
			// Open while the pages and toasts are built, so trophies decode their icons.
			const ImageCacheScope image_scope(image_cache);
			DrawSystemUi(frame, theme, assets, nav);
			// Live toasts only on the Toast page, over it at the top right as toasts sit over a
			// game, so they never cover the specimens of another page. While another page shows,
			// the demo neither publishes nor draws, and its toasts' time runs on.
			if (nav.page == GalleryPage::Toast) {
				const ImVec2 display = ImGui::GetIO().DisplaySize;
				toast_demo.Draw(*ImGui::GetForegroundDrawList(),
				                {.width = display.x, .height = display.y}, theme, assets.fonts,
				                ClockContext::Now(), {.reduced = nav.reduced_motion});
			}
		}
		ImGui::Render();
		if (host.Render(ImGui::GetDrawData(), BACKGROUND) == FrameResult::Fatal) {
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

// Brings Vulkan and ImGui up for `window`, runs the gallery and tears both down in the order
// the backends need, with ImGui's context still alive while the Vulkan backend shuts down.
int RunWindow(SDL_Window* window) {
	VulkanHost host;
	if (!host.Init(window)) {
		return EXIT_FAILURE;
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	// Named once, so the fonts and every frame come from the same theme.
	const ThemeContext& theme  = DEFAULT_THEME;
	const std::string   folder = AssetsFolder();
	// Held so it can be destroyed between the backend shutting down and the context going away,
	// which is the only order the store allows.
	std::optional<ImageStore> images;
	images.emplace(GALLERY_IMAGE_CAPACITY);
	// The system UI, whose toasts the demo draws. Its fonts and the images decoded from their
	// bytes, such as the trophies' icons, are made here with the context current and released for
	// the same reason as the store. A font that can't be loaded is logged and text falls back, so
	// the gallery still runs.
	Instance      system_ui(theme, {.fonts  = {.assets_folder = folder},
	                                .images = {.capacity = GALLERY_DECODED_IMAGE_CAPACITY},
	                                .trophy = {.subtitle = std::string(DEMO_TROPHY_SUBTITLE),
	                                           .hold_ms  = DEMO_TROPHY_HOLD_MS}});
	GalleryAssets assets {.fonts = system_ui.Fonts()};
	LoadGalleryIcon(*images, folder, assets);
	const bool sdl_backend_up = ImGui_ImplSDL3_InitForVulkan(window);
	int        result         = EXIT_FAILURE;
	if (sdl_backend_up && host.InitImGuiBackend()) {
		result = RunLoop(window, host, theme, assets, *images, system_ui);
	} else {
		std::fprintf(stderr, "system_ui_gallery: could not start the ImGui backends\n");
	}
	host.Shutdown();
	system_ui.ReleaseRendering();
	images.reset();
	if (sdl_backend_up) {
		ImGui_ImplSDL3_Shutdown();
	}
	ImGui::DestroyContext();
	return result;
}

} // namespace

// Opens the gallery's window and runs it until it is closed.
int main() {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "system_ui_gallery: SDL_Init failed (%s)\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	SDL_Window* window = SDL_CreateWindow(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT,
	                                      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	int         result = EXIT_FAILURE;
	if (window == nullptr) {
		std::fprintf(stderr, "system_ui_gallery: could not open a window (%s)\n", SDL_GetError());
	} else {
		result = RunWindow(window);
		SDL_DestroyWindow(window);
	}
	SDL_Quit();
	return result;
}
