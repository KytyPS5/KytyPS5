/** Catches a render-time throw anywhere below it and shows something the
 * user can act on, instead of the black window they got before.
 *
 * main.tsx wraps the initial `createRoot().render` in try/catch, but that
 * only covers the first synchronous render. Anything that throws later --
 * a bad value arriving from an invoke, a null the view did not expect --
 * unmounts the whole tree and leaves an empty document. On a launcher
 * driven by a gamepad, with no devtools in a release build, there is no way
 * out of that except killing the process from Task Manager.
 *
 * Deliberately plain and dependency-free below this point:
 *
 * - No `useT()`. If the failure is in i18n (a malformed catalog, a bad
 *   locale import) then translating the error screen is exactly what
 *   cannot be relied on. English, always, so this can never be the thing
 *   that throws while reporting a throw.
 * - Explicit colours rather than theme tokens, for the same reason.
 * - Reload rather than "try again": re-rendering the same broken state
 *   usually just throws again, and a reload is the one recovery that
 *   reliably clears it.
 *
 * The stack is shown rather than hidden. Every bug report this project
 * gets asks for one, and a user who cannot copy it out of a dead window
 * cannot file a useful report.
 */

import { Component, type ErrorInfo, type ReactNode } from "react";

interface Props {
  children: ReactNode;
}

interface State {
  error: Error | null;
}

export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    // Goes to the webview console in a dev build, and is the only record of
    // a crash in a release one -- the UI below shows the same text.
    console.error("Launcher crashed:", error, info.componentStack);
  }

  render(): ReactNode {
    const { error } = this.state;
    if (!error) return this.props.children;

    return (
      <div
        role="alert"
        style={{
          position: "fixed",
          inset: 0,
          display: "flex",
          flexDirection: "column",
          alignItems: "center",
          justifyContent: "center",
          gap: 16,
          padding: 32,
          background: "#0b0d14",
          color: "#e8ecf5",
          fontFamily: "system-ui, sans-serif",
          textAlign: "center",
        }}
      >
        <h1 style={{ margin: 0, fontSize: 22, fontWeight: 600 }}>The launcher hit an error</h1>
        <p style={{ margin: 0, maxWidth: 560, fontSize: 14, opacity: 0.75, lineHeight: 1.5 }}>
          Reloading usually clears it. The emulator itself is unaffected — a game that is already
          running keeps running.
        </p>
        <pre
          style={{
            maxWidth: "min(720px, 90vw)",
            maxHeight: "30vh",
            overflow: "auto",
            margin: 0,
            padding: 12,
            textAlign: "left",
            fontSize: 12,
            lineHeight: 1.45,
            background: "rgba(255,255,255,0.05)",
            border: "1px solid rgba(255,255,255,0.12)",
            borderRadius: 8,
            whiteSpace: "pre-wrap",
            wordBreak: "break-word",
          }}
        >
          {error.stack ?? `${error.name}: ${error.message}`}
        </pre>
        <button
          autoFocus
          onClick={() => window.location.reload()}
          style={{
            padding: "10px 22px",
            fontSize: 14,
            fontWeight: 600,
            color: "#0b0d14",
            background: "#e8ecf5",
            border: "none",
            borderRadius: 999,
            cursor: "pointer",
          }}
        >
          Reload launcher
        </button>
      </div>
    );
  }
}
