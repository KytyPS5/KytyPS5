/** The 18 UI languages, chosen as a subset of the PS5 console-language table
 * that already lives in components/ConfigForm.tsx's CONSOLE_LANGUAGES —
 * consoleLanguageId is that array's index, the same value sent to the
 * emulator as --console-language (src-tauri/src/emulator.rs). A profile's
 * language pick therefore drives both the launcher UI and the console
 * language its games see (see lib/profiles.ts). */
export interface Language {
  code: string;
  /** Name in its own language, for the picker. */
  endonym: string;
  englishName: string;
  consoleLanguageId: number;
  rtl?: boolean;
}

export const LANGUAGES: Language[] = [
  { code: "en", endonym: "English", englishName: "English", consoleLanguageId: 1 },
  { code: "zh-Hans", endonym: "简体中文", englishName: "Chinese (Simplified)", consoleLanguageId: 11 },
  { code: "zh-Hant", endonym: "繁體中文", englishName: "Chinese (Traditional)", consoleLanguageId: 10 },
  { code: "es", endonym: "Español", englishName: "Spanish", consoleLanguageId: 3 },
  { code: "pt-BR", endonym: "Português (Brasil)", englishName: "Portuguese (Brazil)", consoleLanguageId: 17 },
  { code: "ru", endonym: "Русский", englishName: "Russian", consoleLanguageId: 8 },
  { code: "ja", endonym: "日本語", englishName: "Japanese", consoleLanguageId: 0 },
  { code: "de", endonym: "Deutsch", englishName: "German", consoleLanguageId: 4 },
  { code: "fr", endonym: "Français", englishName: "French", consoleLanguageId: 2 },
  { code: "ko", endonym: "한국어", englishName: "Korean", consoleLanguageId: 9 },
  { code: "it", endonym: "Italiano", englishName: "Italian", consoleLanguageId: 5 },
  { code: "tr", endonym: "Türkçe", englishName: "Turkish", consoleLanguageId: 19 },
  { code: "vi", endonym: "Tiếng Việt", englishName: "Vietnamese", consoleLanguageId: 28 },
  { code: "pl", endonym: "Polski", englishName: "Polish", consoleLanguageId: 16 },
  { code: "nl", endonym: "Nederlands", englishName: "Dutch", consoleLanguageId: 6 },
  { code: "th", endonym: "ไทย", englishName: "Thai", consoleLanguageId: 27 },
  { code: "id", endonym: "Bahasa Indonesia", englishName: "Indonesian", consoleLanguageId: 29 },
  { code: "ar", endonym: "العربية", englishName: "Arabic", consoleLanguageId: 21, rtl: true },
];

export const DEFAULT_LANGUAGE_CODE = "en";
