// Link stub for unit tests that compile ResourceMaterialization.cpp without the emulator's config module.
#include "common/emulatorConfig.h"

namespace Config {
bool RaytracingEnabled() { return false; }
} // namespace Config
