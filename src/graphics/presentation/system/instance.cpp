#include "graphics/presentation/system/instance.h"

#include "graphics/presentation/system/imgui/fonts.h"
#include "graphics/presentation/system/views/notification/consumer.h"
#include "graphics/presentation/system/views/trophyUnlocked/consumer.h"
#include "imgui.h"

#include <memory>
#include <string>
#include <utility>

namespace Libs::Graphics::SystemUi {

// The paths are declared before the options, so they read the folder before it is moved.
Instance::Instance(const ThemeContext& theme, InstanceOptions options)
    : m_regular_path(options.fonts.assets_folder + std::string(theme.fonts.regular)),
      m_bold_path(options.fonts.assets_folder + std::string(theme.fonts.bold)),
      m_options(std::move(options)) {
	m_toasts.AddConsumer(std::make_unique<TrophyEventConsumer>(m_toasts, m_options.trophy.subtitle,
	                                                           m_options.trophy.hold_ms));
	m_toasts.AddConsumer(std::make_unique<NotificationEventConsumer>(m_toasts));
}

const FontsContext& Instance::Fonts() {
	PrepareRendering();
	return *m_fonts;
}

ImageCache& Instance::Images() {
	PrepareRendering();
	return *m_images;
}

void Instance::ReleaseRendering() {
	m_images.reset();
	m_fonts.reset();
}

void Instance::PrepareRendering() {
	if (m_images) {
		return;
	}
	m_fonts = LoadFontsContext(*ImGui::GetIO().Fonts, m_regular_path, m_bold_path,
	                           m_options.fonts.merge_fallbacks);
	m_images.emplace(m_options.images.capacity);
}

} // namespace Libs::Graphics::SystemUi
