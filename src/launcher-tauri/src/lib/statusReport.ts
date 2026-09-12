/** Builds a prefilled "Game Emulation Status Report" for the upstream issue
 * tracker.
 *
 * Nearly every issue on KytyPS5/KytyPS5 is a [GAME STATUS] or [GAME BUG]
 * report, and they all get typed out by hand -- while the launcher is
 * sitting on most of what the form asks for. The game's title and serial
 * come from its own param.json, and the emulator build from the binary the
 * launcher just ran, which is the field a hand-written report most often
 * gets wrong or stale. Filling those in is the difference between a report
 * that can be acted on and one that has to be chased.
 *
 * Deliberately opens the form rather than submitting anything: this returns
 * a URL, the caller opens it in the browser, and the user reads, completes
 * the parts only they know (what actually happened, steps, hardware) and
 * presses submit themselves. Nothing is posted on their behalf.
 *
 * Field names are the `id:`s of .github/ISSUE_TEMPLATE/kytyps5-game-emulation.yaml.
 * GitHub silently ignores a parameter whose id it does not recognise, so if
 * the template is renamed upstream this degrades to a blank form rather
 * than failing.
 */

import type { GameStatus } from "../types";

const ISSUE_URL = "https://github.com/KytyPS5/KytyPS5/issues/new";
const TEMPLATE = "kytyps5-game-emulation.yaml";

/** The template's dropdown takes its option labels verbatim, and they are
 * not spelled the way GameStatus is. An unmatched value would leave the
 * dropdown unset, so "Unknown" maps to nothing on purpose -- the user picks
 * it themselves rather than having a guess pre-selected for them. */
const STATUS_OPTION: Record<GameStatus, string | null> = {
  Unknown: null,
  DoesntBoot: "Doesn't boot",
  Logo: "Logo",
  MainMenu: "Main menu",
  InGame: "In game",
};

/** Coarse host OS for the `os` field, from the webview's own UA. Good enough
 * to save typing "Windows" and no more -- the user still fills in the build
 * number, and every other hardware field, which nothing here can see. */
function hostOs(): string {
  const ua = navigator.userAgent;
  if (ua.includes("Windows")) return "Windows";
  if (ua.includes("Mac OS X") || ua.includes("Macintosh")) return "macOS";
  if (ua.includes("Linux")) return "Linux";
  return "";
}

export interface StatusReportInput {
  name: string;
  titleId: string;
  /** The running emulator's self-reported build, e.g.
   * "KytyPS5-2026-08-16-bc2f077". Empty when it could not be probed. */
  emulatorVersion: string;
  status: GameStatus;
}

export function buildStatusReportUrl(input: StatusReportInput): string {
  const params = new URLSearchParams({ template: TEMPLATE });

  // The template's own title prefix; GitHub appends nothing of its own.
  params.set("title", `[GAME STATUS]: ${input.name || input.titleId}`);
  if (input.name) params.set("game-title", input.name);
  if (input.titleId) params.set("game-id", input.titleId);
  if (input.emulatorVersion) params.set("kyty-version", input.emulatorVersion);

  const option = STATUS_OPTION[input.status];
  if (option) params.set("compatibility-status", option);

  const os = hostOs();
  if (os) params.set("os", os);

  return `${ISSUE_URL}?${params.toString()}`;
}
