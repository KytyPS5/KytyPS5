#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_STATUS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_STATUS_H_

#include <cstdint>

// The one way the system UI says how something went, shared by toasts, notices and glyphs. Only the
// standard library, so the emulator's libraries can publish a notice with a status without
// depending on the system UI.
namespace Libs::Graphics::SystemUi {

/// How something went, which sets its colour and its glyph. Info tells, Warning cautions, Error
/// reports a failure and Success confirms.
enum class Status : uint8_t { Info, Warning, Error, Success };

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_STATUS_H_
