#pragma once

// Tiny global flag so the CLI flag (parsed in main.cpp) can be read
// from wherever the game is actually loaded (systemContent / loader flow),
// without those files needing to know about argv parsing.

namespace Loader {

inline bool& DiscordRpcEnabledFlag() {
    static bool enabled = false; // default OFF
    return enabled;
}

inline void SetDiscordRpcEnabled(bool value) {
    DiscordRpcEnabledFlag() = value;
}

inline bool IsDiscordRpcEnabled() {
    return DiscordRpcEnabledFlag();
}

} // namespace Loader
