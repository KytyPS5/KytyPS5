# Contributing

This page covers the process: building, running the tests and the gallery, adding a primitive,
component or gallery page, testing, and commits. The rules for the code itself are in
[CONVENTIONS.md](../CONVENTIONS.md). Read that first. This page links to it rather than repeating it.

## Building

The steps in this section are the Windows ones: the emulator builds on Windows with the Visual Studio 2022
Build Tools, using the x64 clang-cl toolchain and Ninja. On other platforms, follow the project's general build
instructions. The system UI's own targets are the same wherever the project builds.

### Environment

- **Developer environment.** Run CMake from the x64 developer environment. Either open the
  "x64 Native Tools Command Prompt for VS 2022", or run `vcvars64.bat` from the Build Tools folder first.
  Outside it, CMake can find a different linker and fail to configure. Git Bash needs a small wrapper batch
  file to run the same commands.
- **Toolchain.** clang-cl, lld-link and llvm-lib from the Build Tools' LLVM folder, with Ninja as the
  generator. `<VS>` below means your Build Tools folder, usually
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`.
- **Qt.** Point `CMAKE_PREFIX_PATH` at your Qt 6 install for the MSVC 2022 64-bit kit.
- **Certificates.** Set `SSL_CERT_FILE` to the CA bundle that ships with Git for Windows before you configure.
- **ccache.** Leave the compiler launchers empty. ccache breaks header dependency tracking with clang-cl, so
  a header change would not rebuild what includes it.

### Configure

Configure once, passing the full tool paths on every run, so a reconfigure never resets the cache. The
build directory is `_Build/windows`.

```text
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=<VS>/VC/Tools/Llvm/x64/bin/clang-cl.exe
  -DCMAKE_CXX_COMPILER=<VS>/VC/Tools/Llvm/x64/bin/clang-cl.exe
  -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
  -DCMAKE_LINKER=<VS>/VC/Tools/Llvm/x64/bin/lld-link.exe
  -DCMAKE_AR=<VS>/VC/Tools/Llvm/x64/bin/llvm-lib.exe
  -DCMAKE_PREFIX_PATH=<your Qt 6 msvc2022_64 folder>
```

Write it as one line in your shell. The line breaks above are for reading only.

### Build targets

The system UI's test and gallery executables are `EXCLUDE_FROM_ALL`, so a plain build doesn't make them.
Name the target.

| Target | Builds | Command |
|---|---|---|
| `system_ui_gallery` | The gallery window | `cmake --build _Build/windows --target system_ui_gallery` |
| `system_ui_layout_tests` (and the other `system_ui_*_tests`) | One test executable | `cmake --build _Build/windows --target system_ui_layout_tests` |
| `kyty_tests` | Every unit test executable, the system UI's included | `cmake --build _Build/windows --target kyty_tests` |
| `launcher` | The Qt launcher | `cmake --build _Build/windows --target launcher` |

The framework is the `system_ui` library. The emulator, the gallery and most of the system UI suites link it.
`system_ui_event_channel_tests` does not, since its channel is header-only. The library is built only when
something links it. `system_ui` globs every `.cpp` and `.h` under `presentation/system/` except `gallery/`, so a
new file there is picked up with no CMake edit. The `.md` files in `presentation/system/` and its `docs/`
folder are not part of the build.

## Running the tests

```text
cmake --build _Build/windows --target kyty_tests
ctest --test-dir _Build/windows --output-on-failure
```

To run one suite, build its target and select it by name:

```text
cmake --build _Build/windows --target system_ui_layout_tests
ctest --test-dir _Build/windows -R system_ui_layout --output-on-failure
```

The system UI's suites:

| ctest name | Target | Source | What it covers |
|---|---|---|---|
| `system_ui_event_channel` | `system_ui_event_channel_tests` | `tests/SystemUiEventChannelTests.cpp` | Fan-out, capacity, unsubscribing, publishing before subscribing, threads |
| `system_ui_toasts` | `system_ui_toasts_tests` | `tests/SystemUiToastsTests.cpp` | Queue holds, merges by key, dismissal, consumers, `Instance` |
| `system_ui_layout` | `system_ui_layout_tests` | `tests/SystemUiLayoutTests.cpp` | Insets, overflow clips, glyphs against boxes, status and trophy glyphs, component defaults, shadows, glows and shines |
| `system_ui_gradient` | `system_ui_gradient_tests` | `tests/SystemUiGradientTests.cpp` | Gradient position and blending, and boxes drawn with them |
| `system_ui_animation` | `system_ui_animation_tests` | `tests/SystemUiAnimationTests.cpp` | Slides, shines and reduced motion |
| `system_ui_scroll` | `system_ui_scroll_tests` | `tests/SystemUiScrollTests.cpp` | Scrolling from the wheel and pointer |
| `system_ui_gallery` | `system_ui_gallery_tests` | `tests/SystemUiGalleryTests.cpp` | Gallery formatting and contrast arithmetic |

The ctest name `system_ui_gallery` runs `system_ui_gallery_tests`. The target `system_ui_gallery` is the
window program. The two names are different things.

The system UI suites run on the CPU. The ones that paint do so into ImGui's draw lists directly, and none
touches Vulkan.
GPU tests in the wider project must run serially, so a run that includes them should use `ctest -j1`. The
project's `CMakeLists.txt` sets no `RUN_SERIAL` property, so this is a rule for whoever runs ctest. CMake
does not enforce it.

Every suite must be in the `kyty_tests` target's `DEPENDS` list, or a plain `kyty_tests` build won't build it.

## The gallery

The gallery is a window that shows the framework's pages with no game running.

```text
cmake --build _Build/windows --target system_ui_gallery
```

Run the executable the build produces, from the build tree. It loads its fonts and images from an `assets/`
folder beside it. The build copies those in as dependencies of the gallery target. If the fonts don't load,
the gallery still runs, and text falls back to ImGui's own font. The log says so.

The gallery window is 1920 by 1080 and resizable. Its sidebar lists the pages under Foundations,
Motion, Primitives and Components. The page pane scrolls with the wheel. Shift turns the wheel horizontal.

The gallery is built from the public framework. Its pages must not include element internals or reach
into a component's private state. If a page needs something the framework doesn't expose, add the
exposure to the framework, with the convention's rules, and don't work around it in the gallery.

There is one exception, and it is in the gallery's own frame code. `gallery/main.cpp` owns the frame and
includes `element/frame/frameBuilder.h`. `gallery/components/nav.cpp`'s `RememberNav` reads `frame.LayoutOf` after the
frame is arranged, to record each sidebar row's rectangle. The next frame's pointer is then hit-tested
against it. Apart from element kinds' own hooks, such as `Animate`'s, this is the one place that reads layout
records. Pages and components must not do the same.

## Adding a primitive

A primitive is an element kind: it has its own props type, its own hooks, and is built by a builder that
takes props as one struct. If the new thing is only a combination of existing primitives, make it a
component instead.

1. **Folder and files.** Create `element/primitives/<name>/` with `<name>.h` and `<name>.cpp`. Put the
   hooks in `layout.h` and `paint.h` beside them if they are long, and say at the top of each that it is
   internal to the primitive. Short hooks can stay in the `.cpp`, in an anonymous namespace.
2. **Props.** Declare `XProps` with the fields that hold content at the top, and a nested `Self` holding
   `layout` (an `ItemLayout`) and `styles`. Declare `using XElement = Element<XProps>;`. Each struct, alias and
   builder gets a `///` comment, and each field a `//` comment.
3. **Builder.** `[[nodiscard]] XElement X(const XProps& props, ...)`. It calls
   `CurrentBuilder().SetFrameHooks<XProps>(X_FRAME_HOOKS)` on every call, then
   `frame.AddNode(props, children, props.self.layout)`. It takes nothing but its props and children.
   If the frame keeps state for the element, it finds it by a `Key key` in the props. Look at `Text`
   for the smallest example and `Box` for the one with every hook.
4. **Hooks.** `MeasureX` returns a `Measured`. `ArrangeX` places what is below the element and returns
   false when it can't. `PaintX` draws only the element itself, since the walk paints the children. Read
   the element's own data with `frame.FindAs<XProps>(element)`, and its layout with `frame.LayoutOf`.
   Hooks take the `FrameBuilder&` they are given, not `CurrentBuilder()`.
5. **Umbrella.** Add the header to the umbrella `element.h`, which sits at the root of `system/` (not the
   `element/element.h` that declares the handle type), since that is what builders include.
6. **Tests and gallery.** Add a test suite and a gallery page, as below.

## Adding a component

A component builds primitives with a fixed look.

1. **Header.** In `components/`, declare `XTokens` and `XTokens XDefaults(const ThemeContext& theme)`. Give the
   struct a `///` comment and each of its fields a `//` comment, as
   [CONVENTIONS.md](../CONVENTIONS.md#documentation) requires and `CardTokens` shows. Declare `XProps` with a
   nested `Self` for its layout and styles. The builder is
   `XElement X(const XProps& props, const Children& children = {})`. If it scrolls, give its props a
   `Key key` and pass it to its box, and key an animation the same way. Taking `const Children&` once
   gives callers both braces and runs, and the component passes `children` on as they are, as `Card`
   does. A component's builder returns the element of the primitive it is built on: `BoxElement` for a
   box-based component such as `Card` or `Toast`, and `GlyphElement` for a glyph component such as
   `StatusGlyph`.
2. **Implementation.** Read the theme with `CurrentThemeContext()`. Build the look from `XDefaults`, and
   patch the caller's `tokens` over it with `Patch`. A component that is a box with a look patches the
   caller's `self.styles` instead, as `Card` does, and has no `tokens`. A tokens struct with more than
   `MAX_PATCH_FIELDS` (16) fields must group them into nested structs. A plain field left at zero keeps
   the default, so it can't be patched to zero.
3. **Includes.** Include the umbrella headers (`core.h`, `element.h`, `animation.h`) rather than
   primitive headers, as [CONVENTIONS.md](../CONVENTIONS.md#headers) requires.
4. **Glyph-like components** take their colour in props and have no tokens. Their shapes are their look.
5. **Gallery and tests.** Show the component's token values on a gallery page, read from `XDefaults`. Test
   that the defaults are what the component draws, and that a patch changes only the fields it sets.
   `TestCardReadsDefaults` in `tests/SystemUiLayoutTests.cpp` is the model for the first. There is no model
   for the patch test yet.

## Adding a gallery page

1. **Files.** Create `gallery/pages/<name>Page.h`, declaring `[[nodiscard]] AnyElement Build<Name>Page();`,
   and `<name>Page.cpp`. Build the page with `Page({.title, .subtitle}, {...})` and the
   pieces in `components/page.h`, such as `Section`, `TokenLine` and `Lines`. `pages/cardPage.cpp` is a short, complete example.
2. **Registry.** In `gallery/pages/registry.h`, add the page to the `GalleryPage` enum and its `PageEntry` to
   `PAGES` in its group, and bump `GALLERY_PAGE_COUNT`. `PAGES` is indexed by `GalleryPage`, so the enumerator
   must sit at the same position as its entry. Nothing checks that, and a mismatch shifts every sidebar title
   after it by one. If the page becomes the last enumerator, point the count `static_assert` at it, as it
   currently names `GalleryPage::Glyphs`. The `static_assert`s catch a page missing from the count and a group
   split in two.
3. **Dispatch.** In `gallery/main.cpp`, include the header and add a case to `BuildPage`.
4. **Build list.** Add the new `.cpp` to the `system_ui_gallery` target's source list in `CMakeLists.txt`.
   Unlike the `system_ui` library, the gallery's sources are listed one by one, so a new page isn't built
   until it is listed.
5. **Check it.** Build the gallery, open the page, and look at it at the reference size and at a larger
   one. A page's specimens should match the values the component draws, since they come from `XDefaults`.

## Testing expectations

The suites are plain executables, not a test framework. Each file has a small `Check` helper that reports a
failed condition and stops the run, a set of `TestX()` functions, and a `main` that calls them in order. Add a
new `TestX()` to the file that covers its code, then call it from `main`.

Tests must prove what they claim. The project's rule for this, from the project instructions, has three
parts.

- **Name the alternatives.** For each assertion, name what else in the code could make it pass. A glyph that
  ignores its fade and a set of boxes that also ignore theirs would match each other. So `CheckFades` also
  checks that the fade changed the colour. Without that, the test could pass with no fade at all.
- **Compare against a baseline.** Where something else could satisfy the assertion, compare the behaviour under
  test with a baseline that differs only in that behaviour.
- **Mutation-check each new test.** Break the behaviour the test is about, in the code, and confirm the test
  fails. The failure must be the feature going missing, not just being wrong in some other way. A test that
  only fails when a value is wrong doesn't catch a feature that was dropped. Record the mutation and the result
  in the change's notes.

Restore a mutated file from a saved copy, not from git, so other uncommitted edits in the file survive. Rebuild
before you run ctest again, since a test binary built from the mutated code is stale.

Glyphs have their own exact test. `PaintedDraw` runs a glyph's paint into an `ImDrawList`, and `SameDraw` compares
every vertex's position, texture coordinate and colour, and every draw command's clip. `CheckMatchesBoxes`
compares a glyph with the same shapes laid out as boxes, at several sizes, positions and scales where the
shapes' edges snap differently. It also checks that the boxes drew the shapes in their colour, so two that
draw nothing can't pass. Use the same method for any new primitive that replaces a set of boxes.

Tests run on the CPU. Use a `FrameBuilder` with an explicit `FrameScope`, and `DEFAULT_THEME` with a
`ScaleContext` at the reference size, as `DrawGlyph` does. Pass `FontsContext {}` when no text is measured.
Keep one suite per area, add it to the `kyty_tests` dependencies, and register it with `add_test`.

## Code review and commits

- **Review before committing.** Have each change reviewed before it is committed. Amended, fixup and rebased
  commits get the same review, and a change made in response to review is reviewed again.
- **Run the full suite first.** Build `kyty_tests` and run `ctest` before each commit, not only the suites you
  touched.
- **One change per commit.** A commit does one thing. A refactor and the change it enables are two commits, and
  a refactor goes in its own commit before the change it enables. Keep diffs small.
- **Message style.** The subject is `system ui: <what changed>` in lower case, as the existing history does,
  for example `system ui: draw glyph circles in finer segments`. The body explains why the change was needed,
  not what the diff does.
- **Keep names out.** Commit and PR messages carry no personal names, email addresses or links to sessions.
- **Docs in their own commit.** Changes to these Markdown files go in a commit of their own.

## Known gaps in the code and comments

These are places where the code, the comments or the conventions disagree, or where something is declared and
not built. They are listed so a contributor doesn't build on them by mistake. Fixing one is a small, focused
change, and each one should be its own commit.

1. **Shrinking is documented wrongly.** The `ItemLayout` comments on `grow` and `min_main` say elements never
   shrink. `element/primitives/box/measure.cpp` gives each child a shrink factor of 1 unless it has an explicit
   main size or its box scrolls, so children shrink in proportion to their size, down to their least main
   length. [layout.md](layout.md#how-a-box-gives-its-children-their-length) describes the code. The same
   comments say an element with an explicit main size doesn't grow, but `ToFlexItem` in `measure.cpp` passes
   `grow` through whatever the size, and the flex solver grows any item whose `grow` is above zero. This is
   from reading the code and hasn't been checked by a test.
2. **State scale is declared and not drawn.** `StateStyle::scale` is documented in
   `element/primitives/box/box.h` as shrinking the box about its centre. `interactionStates.h` says it is not
   resolved yet, and no code in `element/` reads it. Either implement it or correct the header.
3. **`AssignIds` doesn't exist.** `FrameView::ElementMetadata::id` is documented as set by `AssignIds` from the
   element's key or position. No such function exists, and nothing sets `id`.
4. **Keys are never set.** `key`, `key_number`, `key_is_text` and `has_key` on `ElementMetadata` are never written,
   so the `DuplicateKey` refusal can't happen, and there is no public way to give an element a key. Either add the
   builder support or remove the fields.
5. **Owner names.** [CONVENTIONS.md](../CONVENTIONS.md#naming) says owners are `XCache`, `XStore` or `XService`. The
   trackers and queues, such as `FadeTracker`, `TweenTracker`, `ScrollTracker` and `ToastQueue`, own state under
   other names. Decide whether to extend the rule or rename them. The same file also contradicts itself here:
   [CONVENTIONS.md](../CONVENTIONS.md#lifetimes-and-threads) calls `Instance` and the consumers owners, while the
   naming rule does not name them. Decide which rule holds, then fix the other.
6. **A field out of place.** `ImageProps::source` sits after `Self`, though it holds what the element shows, which
   the convention places above `Self`. [CONVENTIONS.md](../CONVENTIONS.md#building-elements) says existing fields are
   never reordered, and initialisers don't depend on position. So the fix is to record this one as an exception in
   CONVENTIONS.md, not to move the field.
7. **A token that can't be patched.** `ToastTokens::slide_distance` is documented as having no effect when patched,
   since the service slides the toast. A token that can't be changed should not be a token. Consider moving it into
   the service.
8. **Toast key wording.** `ToastService::Add` says a repeat by key "replaces" the toast, while `ToastOptions::key`
   says it "refreshes" it. If the toast is on screen, the repeat swaps in the new builder and options, restarts
   the hold, fades it back in if it was leaving, and keeps its id and place (`queue.cpp:50-54`). If it is
   waiting, the repeat only swaps in the builder and options and keeps its place in the queue (`queue.cpp:56-58`).
   No sound plays again. Make both comments say that.
9. **A none child refuses its container.** [CONVENTIONS.md](../CONVENTIONS.md#errors) says a none element is
   accepted by every builder and container, which draws nothing for it. That is wrong. `NodeTree::ClaimChildren`
   (`element/tree/nodeTree.cpp:21-24`) fails on a none child with `TreeError::None`, so the whole container is
   refused with `ElementError::None`. Build child lists to hold only what shows.
10. **A bad toast hides every toast.** `service/toast/service.cpp` `BuildStack` passes `BoxElement {}` when a toast
    has no builder, and a refused view gives a none too. That none child refuses the toast's `Animate`, and then the
    whole stack box, so every toast on screen is hidden that frame. The fix is in the service: skip a toast whose
    builder gives none, and don't pass a none child to `Animate` or `Box`.
11. **TextCache is keyed by row, not place.** `element/primitives/text/layout.cpp:87` keys each text's layouts by its
    row in the text table, which follows build order. `Text` takes no key. The `Philosophy`
    section of CONVENTIONS.md says state is kept by key or build order, which overstates this. Either give text a key, or say in CONVENTIONS.md
    that text state is keyed by row.
