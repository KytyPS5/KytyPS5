// In-app folder picker, no native dialog plugin dependency. Backed by the
// browse_folder Tauri command (src-tauri/src/browse.rs). Flags folders that
// look like a game root (a direct eboot.bin) instead of a DLL count.

import { useCallback, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { BrowseResult } from "../types";
import { Modal } from "./Modal";
import { useT } from "../i18n";
import styles from "./BrowserList.module.css";

export function FolderBrowserModal({
  initialPath,
  onPick,
  onClose,
}: {
  initialPath?: string;
  onPick: (path: string) => void;
  onClose: () => void;
}) {
  const t = useT();
  const [result, setResult] = useState<BrowseResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [cwd, setCwd] = useState<string | undefined>(initialPath);

  const load = useCallback((path: string | undefined) => {
    setError(null);
    invoke<BrowseResult>("browse_folder", { path: path ?? null })
      .then(setResult)
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, []);

  useEffect(() => {
    load(cwd);
  }, [cwd, load]);

  return (
    <Modal
      title={t("folderBrowser.title")}
      onClose={onClose}
      width={560}
      footer={
        <>
          <button className="pill-button" onClick={onClose}>
            {t("common.cancel")}
          </button>
          <button
            className="pill-button primary"
            disabled={!result || result.isVirtual}
            onClick={() => result && !result.isVirtual && onPick(result.path)}
          >
            {t("folderBrowser.selectFolder")}
          </button>
        </>
      }
    >
      <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 10 }}>
        <button className="pill-button" onClick={() => setCwd(result?.home)}>
          {t("folderBrowser.homeButton")}
        </button>
        <button className="pill-button" disabled={!result?.parent} onClick={() => setCwd(result?.parent ?? undefined)}>
          {t("folderBrowser.up")}
        </button>
        <span
          style={{
            marginLeft: "auto",
            fontSize: 11,
            color: "var(--text-muted)",
            wordBreak: "break-all",
            textAlign: "right",
          }}
        >
          {result ? (result.isVirtual ? "This PC" : result.path) : "…"}
        </span>
      </div>

      <div className={styles.list} data-scroll-region>
        {error && <p style={{ padding: 16, fontSize: 12, color: "var(--error)" }}>{error}</p>}
        {!error && result?.entries.length === 0 && (
          <p style={{ padding: 16, fontSize: 12, color: "var(--text-muted)" }}>{t("folderBrowser.noSubfolders")}</p>
        )}
        {result?.entries.map((entry) => (
          <div
            key={entry.path}
            role="button"
            tabIndex={0}
            data-focusable
            data-focus-key={entry.path}
            onClick={() => setCwd(entry.path)}
            onDoubleClick={() => onPick(entry.path)}
            className={styles.row}
          >
            <span className={styles.rowName}>{entry.name}</span>
            {entry.looksLikeGame && <span className={styles.tag}>{t("folderBrowser.gameTag")}</span>}
          </div>
        ))}
      </div>
    </Modal>
  );
}
