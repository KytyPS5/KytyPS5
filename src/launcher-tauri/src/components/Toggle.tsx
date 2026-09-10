import styles from "./Toggle.module.css";

/** Shared on/off switch, extracted from ConfigForm.tsx's local `Toggle` (an
 * inline `style` object, which is why it never had a hover state -- an
 * inline style can't express `:hover`). `data-focusable` sits on the label,
 * not the track, so gamepad/keyboard confirm (FocusNav.tsx's "confirm"
 * case calls `el.click()`) fires the same onClick a mouse click on the text
 * does -- before this, no toggle was reachable by anything but a mouse
 * landing on the 38px track itself.
 *
 * `label` is optional: ConfigForm's toggles render it inline next to the
 * track (no separate caption elsewhere), while Settings.tsx's row-based
 * toggles (e.g. AudioCategory) already show the caption as the row's own
 * label and pass none here, to avoid showing the text twice. Either way
 * `ariaLabel` names the control for assistive tech. */
export function Toggle({
  checked,
  onChange,
  label,
  ariaLabel,
}: {
  checked: boolean;
  onChange: (v: boolean) => void;
  label?: string;
  ariaLabel?: string;
}) {
  return (
    <label className={styles.label} data-focusable aria-label={ariaLabel ?? label} onClick={() => onChange(!checked)}>
      <span className={`${styles.track} ${checked ? styles.trackOn : ""}`}>
        <span className={styles.knob} />
      </span>
      {label}
    </label>
  );
}
