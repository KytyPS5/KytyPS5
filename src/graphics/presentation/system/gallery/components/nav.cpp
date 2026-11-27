#include "graphics/presentation/system/gallery/components/nav.h"

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/gallery/components/toggle.h"

#include <optional>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// A row's look, quiet at rest, with the theme's hover and press overlays, and when selected the
// accent tint and the selection bar, so the selected page reads without relying on colour alone.
BoxStyles RowStyle(const ThemeContext& theme) {
	BoxStyles style {.radius = theme.radius.sm};
	style.states.hover.overlay      = theme.colors.state.hover;
	style.states.pressed.overlay    = theme.colors.state.pressed;
	style.states.selected.fill      = theme.colors.accent.tint;
	style.states.selected.selection = true;
	return style;
}

// The row for `page`, its title bold whether selected or not, so its width never shifts.
AnyElement Row(const ThemeContext& theme, GalleryPage page, const NavState& state,
               const PointerInput& pointer) {
	const auto index    = static_cast<size_t>(page);
	const bool selected = state.page == page;
	StateInput input    = PointerState(pointer, state.row_rects[index]);
	input.selected      = selected;
	const Color text    = selected ? theme.colors.text.primary : theme.colors.text.secondary;
	const std::array<AnyElement, 1> label {
	    Text({.text = PAGES[index].title,
	          .self = {.styles = {.type = theme.type.label, .color = text}}}),
	};
	return Box(
	    {.layout = {.align = Align::Center, .padding = DpInsets(Dp(), theme.space.s4)},
	     .self = {.layout = {.height = theme.space.s7}, .styles = RowStyle(theme), .state = input}},
	    label);
}

// A group's caption above its first page, in quiet text a step smaller than the rows, so it reads
// as a heading and not a row to pick.
AnyElement GroupCaption(const ThemeContext& theme, PageGroup group) {
	const std::array<AnyElement, 1> caption {
	    Text({.text = GroupTitle(group),
	          .self = {.styles = {.type  = theme.type.body_small,
	                              .color = theme.colors.text.tertiary}}}),
	};
	return Box({.layout = {.padding = DpInsets(theme.space.s4, theme.space.s4, theme.space.s1,
	                                           theme.space.s4)}},
	           caption);
}

} // namespace

void UpdateNav(NavState& state, const PointerInput& pointer) {
	if (!pointer.released) {
		return;
	}
	for (size_t index = 0; index < GALLERY_PAGE_COUNT; ++index) {
		if (state.row_rects[index].Contains(pointer.position)) {
			state.page = static_cast<GalleryPage>(index);
		}
	}
	if (state.toggle_rect.Contains(pointer.position)) {
		state.reduced_motion = !state.reduced_motion;
		++state.flips;
	}
}

NavElements Nav(const NavProps& props) {
	const ThemeContext&      theme   = CurrentThemeContext();
	const NavState&          state   = props.state;
	const PointerInput&      pointer = props.pointer;
	NavElements              built;
	std::vector<AnyElement>  items;
	std::optional<PageGroup> group;
	// The title sits in from the edge as far as the rows' labels, so they line up.
	items.push_back(Box({.layout = {.padding = DpInsets(Dp(), theme.space.s4)}},
	                    {Text({.text = "Design Library",
	                           .self = {.styles = {.type  = theme.type.title,
	                                               .color = theme.colors.text.primary}}})}));
	for (size_t index = 0; index < GALLERY_PAGE_COUNT; ++index) {
		// A caption before the first page of each group, as the pages of a group sit together.
		if (group != PAGES[index].group) {
			group = PAGES[index].group;
			items.push_back(GroupCaption(theme, *group));
		}
		built.rows[index] = Row(theme, static_cast<GalleryPage>(index), state, pointer);
		items.push_back(built.rows[index]);
	}
	// The space left below the pages, which puts the switch at the sidebar's foot.
	items.push_back(Box({.self = {.layout = {.grow = 1.0f}}}));
	built.toggle = Toggle({.label = "Reduced motion",
	                       .on    = state.reduced_motion,
	                       .flips = state.flips,
	                       .state = PointerState(pointer, state.toggle_rect)});
	items.push_back(built.toggle);
	// The gallery's own chrome beside the page, not a raised card of its content.
	built.root = Box({.layout = {.direction = FlexDirection::Column,
	                             .align     = Align::Stretch,
	                             .gap       = theme.space.s1,
	                             .padding   = DpInsets(theme.space.s4, theme.space.s3)},
	                  .self   = {.layout = {.width = SIDEBAR_WIDTH},
	                             .styles = {.fill = theme.colors.surface.hud}}},
	                 items);
	return built;
}

void RememberNav(NavState& state, const FrameBuilder& frame, const NavElements& built) {
	for (size_t index = 0; index < GALLERY_PAGE_COUNT; ++index) {
		if (const auto* layout = frame.LayoutOf(built.rows[index])) {
			state.row_rects[index] = layout->arranged;
		}
	}
	if (const auto* layout = frame.LayoutOf(built.toggle)) {
		state.toggle_rect = layout->arranged;
	}
}

} // namespace Libs::Graphics::SystemUi::Gallery
