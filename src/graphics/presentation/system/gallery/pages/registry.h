#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_REGISTRY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_REGISTRY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The gallery's pages, in the order the sidebar lists them, and the group each sits in.
namespace Libs::Graphics::SystemUi::Gallery {

/// The gallery's pages, one shown at a time, in sidebar order.
enum class GalleryPage : uint8_t {
	Colours,
	Typography,
	Elevations,
	Spacing,
	Theme,
	Animations,
	Speed,
	Timing,
	Box,
	Image,
	Card,
	Toast,
	Glyphs,
};

/// How many pages the gallery has.
inline constexpr size_t GALLERY_PAGE_COUNT = 13;

/// The sidebar's groups. Foundations are the theme's base tokens and how its tiers fit together,
/// motion how things move, primitives the elements everything is built from, and components what
/// is built from them.
enum class PageGroup : uint8_t {
	Foundations,
	Motion,
	Primitives,
	Components,
};

/// What the sidebar shows for a page, its title and the group it is listed under.
struct PageEntry {
	std::string_view title;
	PageGroup        group;
};

/// Every page's entry, indexed by GalleryPage. Pages of one group sit together, so a group's
/// caption goes before its first page.
inline constexpr std::array<PageEntry, GALLERY_PAGE_COUNT> PAGES {
    PageEntry {"Colours", PageGroup::Foundations},
    PageEntry {"Typography", PageGroup::Foundations},
    PageEntry {"Elevations", PageGroup::Foundations},
    PageEntry {"Spacing", PageGroup::Foundations},
    PageEntry {"Theme", PageGroup::Foundations},
    PageEntry {"Animations", PageGroup::Motion},
    PageEntry {"Speed", PageGroup::Motion},
    PageEntry {"Timing", PageGroup::Motion},
    PageEntry {"Box", PageGroup::Primitives},
    PageEntry {"Image", PageGroup::Primitives},
    PageEntry {"Card", PageGroup::Components},
    PageEntry {"Toast", PageGroup::Components},
    PageEntry {"Glyphs", PageGroup::Components},
};

/// Whether PAGES lists the groups in their order, each group's pages together, so the sidebar shows
/// each group's caption once.
[[nodiscard]] constexpr bool GroupsTogether() {
	for (size_t index = 1; index < PAGES.size(); ++index) {
		if (PAGES[index].group < PAGES[index - 1].group) {
			return false;
		}
	}
	return true;
}

// A page added to the enum and not counted would have no sidebar row, and a page listed out of
// its group would give its group a second caption.
static_assert(static_cast<size_t>(GalleryPage::Glyphs) + 1 == GALLERY_PAGE_COUNT);
static_assert(GroupsTogether());

/// The caption of `group` in the sidebar.
[[nodiscard]] constexpr std::string_view GroupTitle(PageGroup group) {
	switch (group) {
		case PageGroup::Foundations: return "Foundations";
		case PageGroup::Motion: return "Motion";
		case PageGroup::Primitives: return "Primitives";
		case PageGroup::Components: break;
	}
	return "Components";
}

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_REGISTRY_H_
