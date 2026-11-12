#ifndef SETTINGS_ELEVATION_H
#define SETTINGS_ELEVATION_H

#include <QString>

// The launcher keeps its configuration in a file that can sit in a folder the user may not write to
// (for example %ProgramData% on Windows). When it cannot save, it writes the settings to a temporary
// file and starts itself again with administrator rights, passing COPY_SETTINGS_ARG, to copy that
// file to its place.
namespace SettingsElevation {

inline constexpr char COPY_SETTINGS_ARG[] = "--copy-settings";

// Whether the process can ask for elevation on this platform.
[[nodiscard]] bool IsSupported();

// Runs the elevated side: copies `source` over `destination` and, on Windows, gives the user
// `user_sid` (the one who runs the launcher; empty: nobody) full control of it, so later saves need
// no elevation. Only a file named like the configuration file is accepted as destination. Returns
// the exit code of the process.
[[nodiscard]] int CopySettings(const QString& source, const QString& destination,
                               const QString& user_sid);

// Starts the launcher again with administrator rights (UAC) to copy `source` to `destination` and
// waits for it. Returns false with `error` set when the user refuses, or the copy fails; true with
// `error` set when the file was saved but the user could not be given permission to change it.
[[nodiscard]] bool RunElevatedCopy(const QString& source, const QString& destination,
                                   QString* error);

} // namespace SettingsElevation

#endif // SETTINGS_ELEVATION_H
