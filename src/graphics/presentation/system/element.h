#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_H_

// Everything code that builds elements needs, in one include. The element handle and lists of
// children, the frame's scope, laying out and painting a root, refusals, and the Box, Glyph, Image
// and Text primitives. Code that owns a frame also includes frame/frameBuilder.h. How the framework
// is written, exposed and used is in CONVENTIONS.md beside this file.

#include "graphics/presentation/system/element/children.h"
#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/frame/frameScope.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/glyph/glyph.h"
#include "graphics/presentation/system/element/primitives/image/image.h"
#include "graphics/presentation/system/element/primitives/text/text.h"
#include "graphics/presentation/system/element/refusal.h"

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_H_
