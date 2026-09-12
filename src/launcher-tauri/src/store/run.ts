import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import type { Configuration, EmulatorExitInfo, EmulatorLogLine } from "../types";
import { createStore } from "./observable";
import { refreshPlayHistory } from "./playtime";

export const isRunningStore = createStore(false);
export const runningGameStore = createStore<string | null>(null);
export const logLinesStore = createStore<EmulatorLogLine[]>([]);
export const lastExitCodeStore = createStore<number | null>(null);

const MAX_LOG_LINES = 4000;

let listenersReady: Promise<void> | null = null;

/** Wires the emulator-log / emulator-exited events once. Safe to call
 * repeatedly — only the first call attaches listeners. */
export function ensureRunListeners(): Promise<void> {
  if (!listenersReady) {
    listenersReady = (async () => {
      await listen<EmulatorLogLine>("emulator-log", (event) => {
        logLinesStore.update((lines) => {
          const next = [...lines, event.payload];
          return next.length > MAX_LOG_LINES ? next.slice(next.length - MAX_LOG_LINES) : next;
        });
      });
      await listen<EmulatorExitInfo>("emulator-exited", (event) => {
        // Only the in-app launch mode fires this event (spawn_in_app starts
        // the watcher; the external-terminal mode never learns the process
        // exited), so a manual Stop click is recorded separately below.
        const gamePath = runningGameStore.get();
        isRunningStore.set(false);
        runningGameStore.set(null);
        lastExitCodeStore.set(event.payload.code);
        if (gamePath) {
          void invoke("record_play_stop", { gamePath }).then(refreshPlayHistory);
        }
      });
    })();
  }
  return listenersReady;
}

/** Wipes the console. Only ever called from the Console page's own Clear
 * button -- starting a game deliberately does not, so output from the run
 * that just crashed is still there when the launcher comes back. */
export function clearLogs(): void {
  logLinesStore.set([]);
}

export async function runGame(info: Configuration, titleId: string): Promise<void> {
  await ensureRunListeners();
  // A separator rather than a wipe: the previous run's output is usually
  // the reason the user is looking at this page at all.
  if (logLinesStore.get().length > 0) {
    logLinesStore.update((lines) => [...lines, { stream: "stdout", line: "" }, { stream: "stdout", line: `--- ${info.name || info.gamePath} ---` }]);
  }
  lastExitCodeStore.set(null);
  runningGameStore.set(info.gamePath);
  isRunningStore.set(true);
  try {
    // If the launcher window is fullscreen right now, the game should launch
    // fullscreen too, regardless of this game's own saved fullscreen_enabled
    // setting -- a launch-time override, not a persisted config change, so
    // toggling the launcher's fullscreen back off doesn't silently flip the
    // saved per-game setting.
    const launcherFullscreen = await getCurrentWindow()
      .isFullscreen()
      .catch(() => false);
    const launchInfo = launcherFullscreen ? { ...info, fullscreenEnabled: true } : info;
    await invoke("run_game", { info: launchInfo, titleId });
    await refreshPlayHistory();
  } catch (e) {
    isRunningStore.set(false);
    runningGameStore.set(null);
    throw e;
  }
}

export async function stopGame(): Promise<void> {
  const gamePath = runningGameStore.get();
  await invoke("stop_game");
  isRunningStore.set(false);
  runningGameStore.set(null);
  if (gamePath) {
    await invoke("record_play_stop", { gamePath });
    await refreshPlayHistory();
  }
}

export async function syncRunningState(): Promise<void> {
  const running = await invoke<boolean>("is_game_running");
  isRunningStore.set(running);
}
