// In-app image picker for the Settings > Dashboard background "Custom" slot
// -- same browse_folder Tauri command as FolderBrowserModal, just with
// fileExtensions set so image files are listed alongside subfolders and can
// be selected. No native file-dialog plugin dependency, matching this
// app's existing convention (see browse.rs's own doc comment).

import { useCallback, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { Folder, Image as ImageIcon } from "lucide-react";
import type { BrowseResult } from "../types";
import { Modal } from "./Modal";
import { useT } from "../i18n";
import styles from "./BrowserList.module.css";

const IMAGE_EXTENSIONS = ["png", "jpg", "jpeg", "webp"];

export function ImageBrowserModal({
  onPick,
  onClose,
}: {
  onPick: (path: string) => void;
  onClose: () => void;
}) {
  const t = useT();
  const [result, setResult] = useState<BrowseResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [cwd, setCwd] = useState<string | undefined>(undefined);
  const [selectedFile, setSelectedFile] = useState<string | null>(null);

  const load = useCallback((path: string | undefined) => {
    setError(null);
    invoke<BrowseResult>("browse_folder", { path: path ?? null, fileExtensions: IMAGE_EXTENSIONS })
      .then(setResult)
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, []);

  useEffect(() => {
    load(cwd);
  }, [cwd, load]);

  const navigate = (path: string) => {
    setSelectedFile(null);
    setCwd(path);
  };

  return (
    <Modal
      title={t("folderBrowser.imageTitle")}
      onClose={onClose}
      width={560}
      footer={
        <>
          <button className="pill-button" onClick={onClose}>
            {t("common.cancel")}
          </button>
          <button
            className="pill-button primary"
            disabled={!selectedFile}
            onClick={() => selectedFile && onPick(selectedFile)}
          >
            {t("folderBrowser.selectImage")}
          </button>
        </>
      }
    >
      <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 10 }}>
        <button className="pill-button" onClick={() => navigate(result?.home ?? "")}>
          {t("folderBrowser.homeButton")}
        </button>
        <button className="pill-button" disabled={!result?.parent} onClick={() => result?.parent && navigate(result.parent)}>
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
          {result?.path ?? "…"}
        </span>
      </div>

      <div className={styles.list} data-scroll-region>
        {error && <p style={{ padding: 16, fontSize: 12, color: "var(--error)" }}>{error}</p>}
        {!error && result?.entries.length === 0 && (
          <p style={{ padding: 16, fontSize: 12, color: "var(--text-muted)" }}>{t("folderBrowser.noImages")}</p>
        )}
        {result?.entries.map((entry) => (
          <div
            key={entry.path}
            role="button"
            tabIndex={0}
            data-focusable
            data-focus-key={entry.path}
            onClick={() => (entry.isDir ? navigate(entry.path) : setSelectedFile(entry.path))}
            onDoubleClick={() => (entry.isDir ? navigate(entry.path) : onPick(entry.path))}
            className={selectedFile === entry.path ? `${styles.row} ${styles.rowSelected}` : styles.row}
          >
            {entry.isDir ? (
              <Folder size={15} style={{ flex: "none", color: "var(--text-muted)" }} />
            ) : (
              <ImageIcon size={15} style={{ flex: "none", color: "var(--text-muted)" }} />
            )}
            <span style={{ fontSize: 13, fontWeight: entry.isDir ? 600 : 400 }}>{entry.name}</span>
          </div>
        ))}
      </div>
    </Modal>
  );
}
