import { invoke } from "@tauri-apps/api/core";
import { createStore, useStore } from "../store/observable";

/** UI-sound (sfx.ts) and emulator audio-output preferences. Frontend-only
 * persistence (localStorage), same pattern as lib/theme.ts and
 * lib/homeLayout.ts. `outputSink` is a PulseAudio/PipeWire sink NAME (the
 * stable identifier `pactl` uses), not its display description -- see
 * audio.rs's AudioSink for the {name, description} split, mirroring the
 * "store the id, display the name" rule lib/homeLayout.ts's sibling stores
 * already follow for non-audio settings. `null` means "system default". */
export interface AudioSettings {
  sfxEnabled: boolean;
  sfxVolume: number;
  outputSink: string | null;
  /** Capture device a game records from. Same "store the id, display the
   * name" rule as outputSink; `null` means the system default. */
  inputSource: string | null;
}

const STORAGE_KEY = "kyty.audioSettings";
const DEFAULT_SETTINGS: AudioSettings = { sfxEnabled: true, sfxVolume: 1, outputSink: null, inputSource: null };

function readStored(): AudioSettings {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return DEFAULT_SETTINGS;
    const parsed = JSON.parse(raw) as Partial<AudioSettings>;
    return {
      sfxEnabled: typeof parsed.sfxEnabled === "boolean" ? parsed.sfxEnabled : DEFAULT_SETTINGS.sfxEnabled,
      sfxVolume: typeof parsed.sfxVolume === "number" ? Math.min(1, Math.max(0, parsed.sfxVolume)) : DEFAULT_SETTINGS.sfxVolume,
      outputSink: typeof parsed.outputSink === "string" ? parsed.outputSink : null,
      inputSource: typeof parsed.inputSource === "string" ? parsed.inputSource : null,
    };
  } catch {
    return DEFAULT_SETTINGS;
  }
}

const audioSettingsStore = createStore<AudioSettings>(readStored());

/** Direct store access for non-component code (sfx.ts's playSfx, a plain
 * function that cannot call the useAudioSettings hook) -- same pattern as
 * store/library.ts's exported configStore used via `.get()` elsewhere. */
export { audioSettingsStore };

export function useAudioSettings(): AudioSettings {
  return useStore(audioSettingsStore);
}

function persist(next: AudioSettings): void {
  audioSettingsStore.set(next);
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(next));
  } catch {
    // Best-effort persistence only -- see readStored's catch.
  }
}

export function setSfxEnabled(enabled: boolean): void {
  persist({ ...audioSettingsStore.get(), sfxEnabled: enabled });
}

export function setSfxVolume(volume: number): void {
  persist({ ...audioSettingsStore.get(), sfxVolume: Math.min(1, Math.max(0, volume)) });
}

/** Also applies the choice immediately: sets PULSE_SINK on the Rust process
 * for every audio stream (both the launcher's own UI sounds -- WebKitGTK
 * plays <audio> via GStreamer in-process on Linux, unlike a
 * multi-process browser, so this actually reaches it -- and future
 * kyty_emulator launches, see emulator.rs's spawn env). Only takes effect
 * for streams opened after this call; anything already playing keeps its
 * current route until it next restarts. */
export async function setOutputSink(sink: string | null): Promise<void> {
  persist({ ...audioSettingsStore.get(), outputSink: sink });
  await invoke("set_audio_output_sink", { sink });
}

/** The capture half of setOutputSink: sets PULSE_SOURCE, which a game
 * launched afterwards inherits. */
export async function setInputSource(source: string | null): Promise<void> {
  persist({ ...audioSettingsStore.get(), inputSource: source });
  await invoke("set_audio_input_source", { source });
}

/** Re-applies the persisted device choices to the current process -- call
 * once at app startup, since PULSE_SINK/PULSE_SOURCE do not survive a
 * relaunch on their own (they are process environment variables, not system
 * settings). Failures are swallowed: a device that has since been unplugged
 * should leave the app running on the default, not break startup. */
export async function reapplyStoredOutputSink(): Promise<void> {
  const { outputSink, inputSource } = audioSettingsStore.get();
  if (outputSink) await invoke("set_audio_output_sink", { sink: outputSink }).catch(() => undefined);
  if (inputSource) await invoke("set_audio_input_source", { source: inputSource }).catch(() => undefined);
}

export interface AudioSink {
  name: string;
  description: string;
}

export async function listAudioSinks(): Promise<AudioSink[]> {
  return invoke<AudioSink[]>("list_audio_sinks").catch(() => []);
}

export async function listAudioSources(): Promise<AudioSink[]> {
  return invoke<AudioSink[]>("list_audio_sources").catch(() => []);
}

/** False where the OS gives a launcher no way to route a child process's
 * audio (Windows) -- the picker is then informational, and the page says so
 * rather than pretending a choice took effect. */
export async function audioSelectionSupported(): Promise<boolean> {
  return invoke<boolean>("audio_selection_supported").catch(() => false);
}
