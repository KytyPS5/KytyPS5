// A BehaviorSubject in ~40 lines, plus the hook that binds it to React.
// One current value, handed to any subscriber immediately; every later
// change pushed to all of them. `useSyncExternalStore` is React's supported
// way to read from exactly this shape without tearing during concurrent
// rendering.

import { useSyncExternalStore } from "react";

export interface Store<T> {
  /** Current value. Always defined — that's the "Behavior" part. */
  get(): T;
  /** Replace the value and notify subscribers. No-op if Object.is-equal. */
  set(next: T): void;
  /** Derive the next value from the current one. */
  update(fn: (current: T) => T): void;
  /** Subscribe; returns an unsubscribe function. */
  subscribe(listener: () => void): () => void;
}

export function createStore<T>(initial: T): Store<T> {
  let value = initial;
  const listeners = new Set<() => void>();

  return {
    get: () => value,
    set(next: T) {
      if (Object.is(value, next)) return;
      value = next;
      for (const l of [...listeners]) l();
    },
    update(fn) {
      this.set(fn(value));
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },
  };
}

export function useStore<T>(store: Store<T>): T {
  return useSyncExternalStore(store.subscribe, store.get, store.get);
}
