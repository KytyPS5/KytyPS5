import { createStore, useStore } from "../store/observable";
import { configStore, saveConfigAndRescan } from "../store/library";
import { LANGUAGES, getLocaleCode, setLocale } from "../i18n";

/** A local, per-user preset -- frontend-only (localStorage), same pattern as
 * lib/theme.ts. Not routed through the Rust KytyConfig
 * contract: switching the active profile instead PUSHES its keymap/deadzone/
 * language into the single live KytyConfig via saveConfigAndRescan, so
 * "different profiles, different key mappings" is real without touching the
 * Rust side (owner decision, see the redesign plan). */
export type TimeFormat = "24h" | "12h";

export interface LauncherProfile {
  id: string;
  name: string;
  avatarId: AvatarId;
  /** i18n language code -- also resolves to a PS5 consoleLanguage id (see
   * i18n/languages.ts) applied to Configuration.consoleLanguage on switch. */
  localeCode: string;
  /** TopBar clock display (lib/useClock.ts). Explicit, not derived from
   * localeCode or the OS locale -- toLocaleTimeString([], ...) previously
   * picked whatever hour cycle the system locale defaulted to, which is 12h
   * AM/PM on plenty of real systems. Defaults to 24h (readTimeFormat below);
   * profiles saved before this field existed read back as undefined and
   * fall back the same way, no migration needed. */
  timeFormat: TimeFormat;
  /** Mirrors KytyConfig.gamepadKeymap. */
  gamepadKeymap: string[];
  /** Mirrors KytyConfig.gamepadDeadzone. */
  gamepadDeadzone: number;
  /** Mirrors Configuration.hostInputMapping (global). */
  hostInputMapping: string[];
}

/** Tolerates profiles persisted before `timeFormat` existed. */
export function readTimeFormat(profile: Pick<LauncherProfile, "timeFormat"> | null): TimeFormat {
  return profile?.timeFormat === "12h" ? "12h" : "24h";
}

// A small fixed palette of decorative avatars -- flat color + the profile's
// initial letter, not generated art. Keeps profile creation instant and
// dependency-free; swap for generated glyphs later if wanted.
export const AVATAR_IDS = ["nova", "ember", "glacier", "orchid", "moss", "solar"] as const;
export type AvatarId = (typeof AVATAR_IDS)[number];

const PROFILES_KEY = "kyty.profiles";
const ACTIVE_ID_KEY = "kyty.activeProfileId";

function readStoredProfiles(): LauncherProfile[] {
  try {
    const raw = localStorage.getItem(PROFILES_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    // Migration: profiles saved before the accent-color picker was removed
    // carry a stale `accent` field. Strip it so it can't linger in
    // localStorage looking like live state.
    return parsed.map((p) => {
      if (p && typeof p === "object" && "accent" in p) {
        const { accent: _accent, ...rest } = p;
        return rest;
      }
      return p;
    });
  } catch {
    // Corrupt/blocked storage -- start empty rather than crash; ensureDefaultProfile reseeds.
    return [];
  }
}

function readStoredActiveId(): string | null {
  try {
    return localStorage.getItem(ACTIVE_ID_KEY);
  } catch {
    return null;
  }
}

function persistProfiles(list: LauncherProfile[]): void {
  try {
    localStorage.setItem(PROFILES_KEY, JSON.stringify(list));
  } catch {
    // Best-effort persistence only -- see readStoredProfiles' catch.
  }
}

function persistActiveId(id: string): void {
  try {
    localStorage.setItem(ACTIVE_ID_KEY, id);
  } catch {
    // Best-effort persistence only.
  }
}

const profilesStore = createStore<LauncherProfile[]>(readStoredProfiles());
const activeProfileIdStore = createStore<string | null>(readStoredActiveId());

function generateId(): string {
  if (typeof crypto !== "undefined" && "randomUUID" in crypto) return crypto.randomUUID();
  return `profile-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function consoleLanguageIdFor(localeCode: string): number {
  return LANGUAGES.find((l) => l.code === localeCode)?.consoleLanguageId ?? 1;
}

/** Pushes a profile's language/keymap/deadzone into the live locale + the
 * single live KytyConfig. Does not touch which profile is "active" -- see
 * setActiveProfile for that. Silently no-ops if the config hasn't loaded yet
 * (App.tsx's boot sequence guarantees it has by the time this can be called
 * from UI, but not necessarily during ensureDefaultProfile's own seeding). */
async function applyProfile(profile: LauncherProfile): Promise<void> {
  setLocale(profile.localeCode);
  const cfg = configStore.get();
  if (!cfg) return;
  await saveConfigAndRescan({
    ...cfg,
    gamepadKeymap: profile.gamepadKeymap,
    gamepadDeadzone: profile.gamepadDeadzone,
    global: {
      ...cfg.global,
      hostInputMapping: profile.hostInputMapping,
      consoleLanguage: consoleLanguageIdFor(profile.localeCode),
    },
  });
}

/** Call once at boot, after the initial config load resolves (App.tsx awaits
 * refreshLibrary() before this) -- so configStore.get() is guaranteed
 * non-null here. Seeds a single profile from whatever is currently in
 * KytyConfig / the active locale, so upgrading to profile-aware Settings
 * never silently resets an existing gamepad setup. No-ops if profiles
 * already exist (every run after the first). */
export function ensureDefaultProfile(): void {
  if (profilesStore.get().length > 0) return;
  const cfg = configStore.get();
  const profile: LauncherProfile = {
    id: generateId(),
    name: "Profile 1",
    avatarId: AVATAR_IDS[0],
    localeCode: getLocaleCode(),
    timeFormat: "24h",
    gamepadKeymap: cfg?.gamepadKeymap ?? [],
    gamepadDeadzone: cfg?.gamepadDeadzone ?? 0,
    hostInputMapping: cfg?.global.hostInputMapping ?? [],
  };
  profilesStore.set([profile]);
  persistProfiles([profile]);
  activeProfileIdStore.set(profile.id);
  persistActiveId(profile.id);
  // Config already reflects these exact values (they were read from it) --
  // no need to round-trip a write on first boot, just adopt the locale.
}

export function useProfiles(): LauncherProfile[] {
  return useStore(profilesStore);
}

export function useActiveProfile(): LauncherProfile | null {
  const list = useStore(profilesStore);
  const activeId = useStore(activeProfileIdStore);
  return list.find((p) => p.id === activeId) ?? list[0] ?? null;
}

/** Switches the active profile: persists the choice, then applies its
 * language and gamepad/input settings to the live app. */
export async function setActiveProfile(id: string): Promise<void> {
  const profile = profilesStore.get().find((p) => p.id === id);
  if (!profile) return;
  activeProfileIdStore.set(id);
  persistActiveId(id);
  await applyProfile(profile);
}

/** Creates a new profile, seeded from the currently live gamepad/input
 * settings (a sensible starting point -- the caller/UI is expected to let
 * the user customize language and remap from there). Does not switch to it;
 * call setActiveProfile separately if that's the desired flow. */
export function createProfile(name: string, avatarId: AvatarId = AVATAR_IDS[0]): LauncherProfile {
  const cfg = configStore.get();
  const profile: LauncherProfile = {
    id: generateId(),
    name,
    avatarId,
    localeCode: getLocaleCode(),
    timeFormat: "24h",
    gamepadKeymap: cfg?.gamepadKeymap ?? [],
    gamepadDeadzone: cfg?.gamepadDeadzone ?? 0,
    hostInputMapping: cfg?.global.hostInputMapping ?? [],
  };
  const next = [...profilesStore.get(), profile];
  profilesStore.set(next);
  persistProfiles(next);
  return profile;
}

/** Renames/edits a profile's fields. If the edited profile is the currently
 * active one, the change is applied to the live app immediately (matches
 * the existing InputMappingDialog behavior of taking effect on save). */
export async function updateProfile(id: string, patch: Partial<Omit<LauncherProfile, "id">>): Promise<void> {
  let updated: LauncherProfile | undefined;
  const next = profilesStore.get().map((p) => {
    if (p.id !== id) return p;
    updated = { ...p, ...patch };
    return updated;
  });
  if (!updated) return;
  profilesStore.set(next);
  persistProfiles(next);
  if (activeProfileIdStore.get() === id) await applyProfile(updated);
}

/** Deletes a profile. Refuses to delete the last remaining one -- there must
 * always be at least one profile. If the deleted profile was active, switches
 * to whichever profile is now first. */
export async function deleteProfile(id: string): Promise<void> {
  const list = profilesStore.get();
  if (list.length <= 1) return;
  const next = list.filter((p) => p.id !== id);
  profilesStore.set(next);
  persistProfiles(next);
  if (activeProfileIdStore.get() === id) await setActiveProfile(next[0].id);
}
