import type { CSSProperties, HTMLAttributes, ReactNode } from "react";
import styles from "./GlassPanel.module.css";

export function GlassPanel({
  variant = "light",
  className,
  style,
  children,
  ...rest
}: {
  /** "light": flat fill, for panels over the (already-blurred) Home hero.
   * "heavy": real backdrop-filter, only for panels over static content.
   * See GlassPanel.module.css's header comment. */
  variant?: "light" | "heavy";
  className?: string;
  style?: CSSProperties;
  children?: ReactNode;
} & Omit<HTMLAttributes<HTMLDivElement>, "className" | "style" | "children">) {
  const variantClass = variant === "heavy" ? styles.heavy : styles.light;
  return (
    <div className={className ? `${variantClass} ${className}` : variantClass} style={style} {...rest}>
      {children}
    </div>
  );
}
