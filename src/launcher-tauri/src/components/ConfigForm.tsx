// Shared emulator-settings form, one field per Configuration property that
// configurationEditDialog.cpp exposes. Used both for the global defaults
// (views/Settings.tsx) and a per-game override (GameDetail.tsx's Settings tab).

import { useEffect, useState } from "react";
import { currentMonitor } from "@tauri-apps/api/window";
import type { Configuration, LogDirection, ProfilerDirection, Resolution, ShaderOptimizationType } from "../types";
import { useConsoleLanguageNames, useT } from "../i18n";
import { Dropdown } from "./Dropdown";
import { Toggle } from "./Toggle";

const RESOLUTION_PRESETS: [number, number][] = [
  [1280, 720],
  [1600, 900],
  [1920, 1080],
  [2560, 1440],
  [3200, 1800],
  [3840, 2160],
];

function encodeResolution(width: number, height: number): Resolution {
  return `R${width}X${height}`;
}

function decodeResolution(value: Resolution): { width: number; height: number } {
  const m = /^R(\d+)X(\d+)$/.exec(value);
  return m ? { width: Number(m[1]), height: Number(m[2]) } : { width: 1280, height: 720 };
}

/** The display's real resolution, so the picker never offers more than the
 * screen can actually show. Falls back to 1920x1080 if Tauri can't report
 * one (e.g. outside a real window). */
function useMaxResolution(): { width: number; height: number } {
  const [max, setMax] = useState({ width: 1920, height: 1080 });
  useEffect(() => {
    void currentMonitor()
      .then((m) => {
        // m.size is physical pixels; dividing by scaleFactor is what makes
        // this the logical resolution the game should actually render at —
        // without it, "native resolution" on a HiDPI Windows display or a
        // Retina Mac is 2x (or more) what the screen logically shows.
        if (m) setMax({ width: Math.round(m.size.width / m.scaleFactor), height: Math.round(m.size.height / m.scaleFactor) });
      })
      .catch(() => {
        // Not running inside a real Tauri window — keep the fallback.
      });
  }, []);
  return max;
}

export function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: "flex", flexDirection: "column", gap: 10, fontSize: 12 }}>
      <span style={{ color: "var(--text-secondary)", fontWeight: 600 }}>{label}</span>
      {children}
    </label>
  );
}

export function ConfigForm({
  value,
  onChange,
}: {
  value: Configuration;
  onChange: (next: Configuration) => void;
}) {
  const t = useT();
  const consoleLanguages = useConsoleLanguageNames();
  const set = <K extends keyof Configuration>(key: K, v: Configuration[K]) =>
    onChange({ ...value, [key]: v });

  const maxRes = useMaxResolution();
  const current = decodeResolution(value.screenResolution);
  const nativeValue = encodeResolution(maxRes.width, maxRes.height);
  const presetOptions = RESOLUTION_PRESETS.filter(([w, h]) => w <= maxRes.width && h <= maxRes.height).map(
    ([w, h]) => encodeResolution(w, h),
  );
  if (!presetOptions.includes(nativeValue)) presetOptions.push(nativeValue);
  const isKnownPreset = presetOptions.includes(value.screenResolution);
  const [forceCustom, setForceCustom] = useState(false);
  const showCustom = forceCustom || !isKnownPreset;

  return (
    <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", rowGap: 26, columnGap: 24 }}>
      <Field label={t("configForm.screenResolution")}>
        <Dropdown
          ariaLabel={t("configForm.screenResolution")}
          value={showCustom ? "custom" : value.screenResolution}
          onChange={(v) => {
            if (v === "custom") {
              setForceCustom(true);
            } else {
              setForceCustom(false);
              set("screenResolution", v as Resolution);
            }
          }}
          options={[
            ...presetOptions.map((v) => {
              const { width, height } = decodeResolution(v);
              const isNative = v === nativeValue;
              return { value: v, label: `${width} × ${height}${isNative ? ` (${t("configForm.native")})` : ""}` };
            }),
            { value: "custom", label: t("configForm.custom") },
          ]}
        />
        {showCustom && (
          <div style={{ display: "flex", alignItems: "center", gap: 8, marginTop: 8 }}>
            <input
              type="number"
              min={1}
              value={current.width}
              onChange={(e) => set("screenResolution", encodeResolution(Number(e.target.value) || 1, current.height))}
              style={{ width: 90 }}
            />
            <span style={{ color: "var(--text-muted)" }}>×</span>
            <input
              type="number"
              min={1}
              value={current.height}
              onChange={(e) => set("screenResolution", encodeResolution(current.width, Number(e.target.value) || 1))}
              style={{ width: 90 }}
            />
          </div>
        )}
      </Field>

      <Field label={t("configForm.vblankFrequency")}>
        <input
          type="number"
          min={1}
          value={value.vblankFrequency}
          onChange={(e) => set("vblankFrequency", Number(e.target.value) || 0)}
        />
      </Field>

      <Field label={t("configForm.consoleLanguage")}>
        <Dropdown
          ariaLabel={t("configForm.consoleLanguage")}
          value={String(value.consoleLanguage)}
          onChange={(v) => set("consoleLanguage", Number(v))}
          options={consoleLanguages.map((name, i) => ({ value: String(i), label: name }))}
        />
      </Field>

      <Field label={t("configForm.shaderOptimization")}>
        <Dropdown
          ariaLabel={t("configForm.shaderOptimization")}
          value={value.shaderOptimizationType}
          onChange={(v) => set("shaderOptimizationType", v as ShaderOptimizationType)}
          options={[
            { value: "None", label: t("common.none") },
            { value: "Size", label: t("configForm.optionSize") },
            { value: "Performance", label: t("configForm.optionPerformance") },
          ]}
        />
      </Field>

      <Field label={t("configForm.shaderLog")}>
        <Dropdown
          ariaLabel={t("configForm.shaderLog")}
          value={value.shaderLogDirection}
          onChange={(v) => set("shaderLogDirection", v as LogDirection)}
          options={[
            { value: "Silent", label: t("configForm.silent") },
            { value: "Console", label: t("nav.console") },
            { value: "File", label: t("configForm.file") },
          ]}
        />
      </Field>
      <Field label={t("configForm.shaderLogFolder")}>
        <input
          type="text"
          disabled={value.shaderLogDirection !== "File"}
          value={value.shaderLogFolder}
          onChange={(e) => set("shaderLogFolder", e.target.value)}
        />
      </Field>

      <Field label={t("configForm.printfOutput")}>
        <Dropdown
          ariaLabel={t("configForm.printfOutput")}
          value={value.printfDirection}
          onChange={(v) => set("printfDirection", v as LogDirection)}
          options={[
            { value: "Silent", label: t("configForm.silent") },
            { value: "Console", label: t("nav.console") },
            { value: "File", label: t("configForm.file") },
          ]}
        />
      </Field>
      <Field label={t("configForm.printfOutputFile")}>
        <input
          type="text"
          disabled={value.printfDirection !== "File"}
          value={value.printfOutputFile}
          onChange={(e) => set("printfOutputFile", e.target.value)}
        />
      </Field>

      <Field label={t("configForm.profiler")}>
        <Dropdown
          ariaLabel={t("configForm.profiler")}
          value={value.profilerDirection}
          onChange={(v) => set("profilerDirection", v as ProfilerDirection)}
          options={[
            { value: "None", label: t("common.none") },
            { value: "Network", label: t("configForm.network") },
          ]}
        />
      </Field>

      <Field label={t("configForm.commandBufferDumpFolder")}>
        <input
          type="text"
          disabled={!value.commandBufferDumpEnabled}
          value={value.commandBufferDumpFolder}
          onChange={(e) => set("commandBufferDumpFolder", e.target.value)}
        />
      </Field>

      {/* A grid, not the previous flex-wrap row -- flex-wrap let the toggle
         count (6) break unevenly across the 720px form width depending on
         label length, so columns never lined up. Two fixed columns keep
         every switch's label and control aligned regardless of wrap. No
         border here, same "spacing carries the grouping" rule as the rest
         of this page (see Settings.module.css's .row comment) -- the
         top gap alone marks this as a new group below the text fields. */}
      <div
        style={{
          gridColumn: "1 / -1",
          display: "grid",
          gridTemplateColumns: "1fr 1fr",
          rowGap: 18,
          columnGap: 24,
          marginTop: 30,
        }}
      >
        <Toggle checked={value.fullscreenEnabled} onChange={(v) => set("fullscreenEnabled", v)} label={t("configForm.fullscreen")} />
        <Toggle
          checked={value.vulkanValidationEnabled}
          onChange={(v) => set("vulkanValidationEnabled", v)}
          label={t("configForm.vulkanValidation")}
        />
        <Toggle
          checked={value.shaderValidationEnabled}
          onChange={(v) => set("shaderValidationEnabled", v)}
          label={t("configForm.shaderValidation")}
        />
        <Toggle
          checked={value.commandBufferDumpEnabled}
          onChange={(v) => set("commandBufferDumpEnabled", v)}
          label={t("configForm.commandBufferDump")}
        />
        <Toggle checked={value.renderdocEnabled} onChange={(v) => set("renderdocEnabled", v)} label={t("configForm.renderdocCapture")} />
        <Toggle
          checked={value.bvhStubEnabled}
          onChange={(v) => set("bvhStubEnabled", v)}
          label={t("configForm.bvhStub")}
        />
      </div>
    </div>
  );
}
