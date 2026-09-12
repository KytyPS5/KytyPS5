import { createStore, useStore } from "../store/observable";

/** Which Home layout to render -- "standard" is the existing growing
 * carousel row; "library" keeps the shortcut row and adds a second row of
 * larger game cards below it, closer to a traditional grid library view.
 * Frontend-only (localStorage), same pattern as lib/theme.ts. */
export type HomeLayout = "standard" | "library";

const STORAGE_KEY = "kyty.homeLayout";
const DEFAULT_LAYOUT: HomeLayout = "library";

function readStored(): HomeLayout {
  try {
    const v = localStorage.getItem(STORAGE_KEY);
    return v === "standard" || v === "library" ? v : DEFAULT_LAYOUT;
  } catch {
    return DEFAULT_LAYOUT;
  }
}

const homeLayoutStore = createStore<HomeLayout>(readStored());

export function useHomeLayout(): HomeLayout {
  return useStore(homeLayoutStore);
}

export function setHomeLayout(layout: HomeLayout): void {
  homeLayoutStore.set(layout);
  try {
    localStorage.setItem(STORAGE_KEY, layout);
  } catch {
    // Best-effort persistence only -- see readStored's catch.
  }
}
