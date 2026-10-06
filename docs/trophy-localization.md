# Trophy localization

The trophy system (unlock toast, trophy overview, trophy viewer and trophy inspector) reads its
interface text from editable language files. Trophy names and descriptions are not part of these
files: they come from the game's own `trophy00.ucp` metadata, already localized by the game.

Other parts of the emulator still use built-in strings. There is one file per language: every system adds its own keys to it under its own prefix (trophy text uses `trophy.`), so other areas can move over later without new files.

## Where the files are

`assets/localization/<locale>.json`, shipped next to the emulator and the launcher
(`KytyPS5.app/Contents/Resources/assets/localization` on macOS). `<locale>` is the console
locale code, for example `en-US`, `es-ES`, `pt-BR`, `fr-CA`, `de-DE`. The console language comes from
the emulator configuration, and from the per-game setting in the launcher.

## Format

A flat UTF-8 JSON object. Each key maps to the text. `{0}`, `{1}`, ... are replaced by values.
Keys of other systems are ignored by the trophy code. Keys that start with `_` (for example `_comment`) are ignored, and so are non-string values.

```json
{
  "_comment": "Spanish",
  "trophy.toast.earned": "¡Ganaste un trofeo!",
  "trophy.overview.earned": "Conseguidos {0}/{1}"
}
```

## Fallback

Missing keys fall back to `en-US.json`, then to the built-in English text. A translation can
therefore contain only some of the keys. A missing or invalid file is skipped.

## Adding a language

1. Copy `en-US.json` to `<locale>.json`, using the locale code from the table in
   `src/common/trophyStrings.cpp` (`LocaleName`).
2. Translate the values and keep the keys and `{n}` placeholders unchanged.
3. Save the file as UTF-8.

No rebuild is needed. Restart the emulator or open the viewer again.

## Keys

| Key | Placeholders |
| --- | --- |
| `trophy.toast.earned` | |
| `trophy.grade.platinum`, `trophy.grade.gold`, `trophy.grade.silver`, `trophy.grade.bronze` | |
| `trophy.hidden` | |
| `trophy.numbered` | `{0}` trophy ID |
| `trophy.viewer.window_title` | |
| `trophy.viewer.window_title_game` | `{0}` game name |
| `trophy.viewer.progress`, `trophy.viewer.earned` | |
| `trophy.viewer.all_trophies` | `{0}` number of trophies |
| `trophy.viewer.no_package` | `{0}` folder |
| `trophy.viewer.unreadable_package` | `{0}` file name |
| `trophy.overview.title` | |
| `trophy.overview.total` | `{0}` earned trophies |
| `trophy.overview.earned` | `{0}` earned, `{1}` total |
| `trophy.overview.empty` | |
| `trophy.inspector.title`, `trophy.inspector.grade`, `trophy.inspector.status`, `trophy.inspector.status_earned`, `trophy.inspector.status_not_earned`, `trophy.inspector.earned_date`, `trophy.inspector.details`, `trophy.inspector.show_hidden` | |
