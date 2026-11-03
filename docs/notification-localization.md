# Notification localization

Trophy notification subtitles use the configured console language. Trophy names still come
from the game's metadata. The launcher trophy dialogs are unchanged.

Edit `assets/localization/<locale>.json`, using flat UTF-8 string values:

```json
{
  "trophy.toast.earned": "Trophy earned!"
}
```

CMake converts the catalogs into C++ byte arrays in the build directory. The emulator parses
the embedded catalogs once and falls back to `en-US.json` for missing translations. No language
files or translation service are needed at runtime. Rebuild after editing a catalog.

For local testing, rebuild `kyty_emulator`, choose a game's Console Language in the launcher,
and unlock a trophy with notifications enabled. Installed system fonts provide non-Latin
glyphs; the selected language needs an appropriate font on the host.
