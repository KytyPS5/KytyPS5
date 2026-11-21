use crate::games::{GameEntry, GameKind};
use egui::{Align2, Color32, CornerRadius, FontFamily, FontId, Painter, Pos2, Rect, Shape, Stroke, StrokeKind, Vec2};

pub fn font_light(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("light".into()))
}

pub fn font_semilight(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("semilight".into()))
}

pub fn font_bold(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("bold".into()))
}

/// Draws a crisp PS5-style rounded card with smooth simulated vertical/radial shading
/// (never leaks sharp corners outside the rounded radius).
pub fn paint_rounded_card_base(
    painter: &Painter,
    rect: Rect,
    radius: u8,
    base_dark: Color32,
    accent: Color32,
) {
    let cr = CornerRadius::same(radius);
    painter.rect_filled(rect, cr, base_dark);

    // Subtle upper-left specular gradient via clipped painter
    let clipped = painter.with_clip_rect(rect);
    let top_glow = Pos2::new(rect.left() + rect.width() * 0.25, rect.top() + rect.height() * 0.18);
    for i in (1..=5).rev() {
        let f = i as f32 / 5.0;
        let r = rect.width().max(rect.height()) * 0.85 * f;
        let a = ((1.0 - f * 0.72) * 58.0) as u8;
        clipped.circle_filled(
            top_glow,
            r,
            Color32::from_rgba_unmultiplied(accent.r(), accent.g(), accent.b(), a),
        );
    }

    // Bottom dark scrim for legibility
    let scrim = Rect::from_min_max(
        Pos2::new(rect.left(), rect.top() + rect.height() * 0.58),
        rect.max,
    );
    clipped.rect_filled(scrim, CornerRadius::ZERO, Color32::from_rgba_unmultiplied(6, 9, 16, 115));

    // Delicate top-edge glass highlight
    clipped.line_segment(
        [
            Pos2::new(rect.left() + radius as f32, rect.top() + 0.8),
            Pos2::new(rect.right() - radius as f32, rect.top() + 0.8),
        ],
        Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 55)),
    );
}

/// Custom procedural cover artwork for PS5 showcase tiles and fallback real games.
pub fn paint_game_tile_artwork(
    painter: &Painter,
    rect: Rect,
    game: &GameEntry,
    selected: bool,
    time: f64,
) {
    let clipped = painter.with_clip_rect(rect);
    let c = rect.center();
    let w = rect.width();
    let h = rect.height();
    let t = time as f32;

    match game.kind {
        GameKind::AddGameTile => {
            let r = w * 0.16;
            let stroke = Stroke::new(2.2_f32, Color32::from_rgba_unmultiplied(235, 242, 255, 230));
            clipped.line_segment([Pos2::new(c.x - r, c.y - 6.0), Pos2::new(c.x + r, c.y - 6.0)], stroke);
            clipped.line_segment([Pos2::new(c.x, c.y - 6.0 - r), Pos2::new(c.x, c.y - 6.0 + r)], stroke);
            clipped.text(
                Pos2::new(c.x, rect.bottom() - 16.0),
                Align2::CENTER_CENTER,
                "Oyun Ekle",
                font_semilight(if selected { 11.5 } else { 9.5 }),
                Color32::from_rgba_unmultiplied(220, 230, 248, 210),
            );
            return;
        }
        GameKind::LibraryTile => {
            // Authentic PS5 3x3 minimal grid icon
            let gap = w * 0.085;
            let cell = w * 0.075;
            for row in -1..=1 {
                for col in -1..=1 {
                    let p = Pos2::new(c.x + col as f32 * (cell + gap), c.y - 6.0 + row as f32 * (cell + gap));
                    clipped.rect_filled(
                        Rect::from_center_size(p, Vec2::splat(cell)),
                        CornerRadius::same(2),
                        Color32::from_rgba_unmultiplied(230, 238, 252, 215),
                    );
                }
            }
            clipped.text(
                Pos2::new(c.x, rect.bottom() - 16.0),
                Align2::CENTER_CENTER,
                "Kütüphane",
                font_semilight(if selected { 11.5 } else { 9.5 }),
                Color32::from_rgba_unmultiplied(220, 230, 248, 210),
            );
            return;
        }
        _ => {}
    }

    // Distinct high-craft vector artwork per showcase title
    if game.title.contains("ASTRO") {
        // Astro Bot iconic glowing cyan visor + white helmet + starry rings
        clipped.circle_filled(
            Pos2::new(c.x, c.y - h * 0.06),
            w * 0.30,
            Color32::from_rgb(240, 245, 255),
        );
        let visor = Rect::from_center_size(Pos2::new(c.x, c.y - h * 0.06), Vec2::new(w * 0.44, h * 0.25));
        clipped.rect_filled(visor, CornerRadius::same(12), Color32::from_rgb(10, 22, 46));
        let eye_y = c.y - h * 0.06;
        let blink = if (t * 0.7).sin() > 0.96 { 0.25 } else { 1.0 };
        for side in [-1.0_f32, 1.0_f32] {
            clipped.rect_filled(
                Rect::from_center_size(
                    Pos2::new(c.x + side * w * 0.095, eye_y),
                    Vec2::new(w * 0.075, h * 0.09 * blink),
                ),
                CornerRadius::same(6),
                Color32::from_rgb(56, 215, 255),
            );
        }
        // Subtle blue cape ring
        clipped.circle_stroke(
            Pos2::new(c.x, c.y - h * 0.06),
            w * 0.38,
            Stroke::new(1.5_f32, Color32::from_rgba_unmultiplied(120, 220, 255, 85)),
        );
    } else if game.title.contains("Demon") {
        // Gothic cathedral arch + glowing golden sword silhouette
        let arch_center = Pos2::new(c.x, c.y - h * 0.06);
        clipped.circle_filled(arch_center, w * 0.26, Color32::from_rgba_unmultiplied(235, 178, 86, 55));
        clipped.circle_stroke(
            arch_center,
            w * 0.32,
            Stroke::new(1.6_f32, Color32::from_rgba_unmultiplied(245, 200, 125, 130)),
        );
        // Sword blade & crossguard
        clipped.line_segment(
            [Pos2::new(c.x, c.y - h * 0.34), Pos2::new(c.x, c.y + h * 0.22)],
            Stroke::new(3.0_f32, Color32::from_rgb(245, 226, 185)),
        );
        clipped.line_segment(
            [Pos2::new(c.x - w * 0.14, c.y - h * 0.04), Pos2::new(c.x + w * 0.14, c.y - h * 0.04)],
            Stroke::new(2.8_f32, Color32::from_rgb(235, 188, 108)),
        );
    } else if game.title.contains("Ratchet") {
        // Dimensional swirling rift portal
        for i in 0..4 {
            let fi = i as f32;
            let r = w * (0.14 + fi * 0.075);
            let alpha = (180 - i * 35) as u8;
            clipped.circle_stroke(
                Pos2::new(c.x, c.y - h * 0.05),
                r,
                Stroke::new(2.2_f32, Color32::from_rgba_unmultiplied(230, 130, 255, alpha)),
            );
        }
        clipped.circle_filled(
            Pos2::new(c.x, c.y - h * 0.05),
            w * 0.11,
            Color32::from_rgba_unmultiplied(255, 220, 130, 220),
        );
    } else if game.title.contains("Returnal") {
        // Eerie dark planet eclipse with glowing teal corona
        let p = Pos2::new(c.x, c.y - h * 0.06);
        clipped.circle_filled(p, w * 0.27, Color32::from_rgba_unmultiplied(54, 225, 205, 85));
        clipped.circle_filled(p, w * 0.21, Color32::from_rgb(7, 15, 22));
        clipped.circle_stroke(
            p,
            w * 0.22,
            Stroke::new(2.2_f32, Color32::from_rgb(90, 245, 225)),
        );
    } else if game.title.contains("Gran Turismo") {
        // Sleek racing speedometer arc + red/white track sweep
        let p = Pos2::new(c.x, c.y + h * 0.04);
        let mut arc_pts = Vec::with_capacity(25);
        for s in 0..=24 {
            let ang = std::f32::consts::PI * (1.15 - (s as f32 / 24.0) * 1.3);
            arc_pts.push(Pos2::new(p.x + ang.cos() * w * 0.30, p.y - ang.sin() * w * 0.30));
        }
        clipped.add(Shape::line(arc_pts, Stroke::new(3.2_f32, Color32::from_rgb(245, 72, 86))));
        clipped.text(
            Pos2::new(c.x, c.y - h * 0.04),
            Align2::CENTER_CENTER,
            "GT7",
            font_bold(if selected { 22.0 } else { 17.0 }),
            Color32::WHITE,
        );
    } else {
        // Real PS5 / ELF / ZAR game without icon0.png
        let tag = match game.kind {
            GameKind::Elf => "ELF",
            GameKind::ZArchive => "ZAR",
            _ => "PS5",
        };
        clipped.circle_stroke(
            Pos2::new(c.x, c.y - h * 0.06),
            w * 0.26,
            Stroke::new(1.5_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 75)),
        );
        clipped.text(
            Pos2::new(c.x, c.y - h * 0.06),
            Align2::CENTER_CENTER,
            tag,
            font_bold(if selected { 18.0 } else { 14.0 }),
            Color32::from_rgba_unmultiplied(255, 255, 255, 225),
        );
    }

    // Clean small PS5 corner pill on top-left of tile
    if !matches!(game.kind, GameKind::AddGameTile | GameKind::LibraryTile) {
        let pill = Rect::from_min_size(Pos2::new(rect.left() + 8.0, rect.top() + 8.0), Vec2::new(30.0, 14.0));
        clipped.rect_filled(pill, CornerRadius::same(4), Color32::from_rgba_unmultiplied(255, 255, 255, 220));
        clipped.text(
            pill.center(),
            Align2::CENTER_CENTER,
            "PS5",
            font_bold(8.5),
            Color32::from_rgb(12, 16, 26),
        );
    }
}

/// Scenic full-screen hero composition painted on the right side of the PS5 home screen
/// when a showcase game is focused (gives that unmistakable PS5 full-bleed poster feel).
pub fn paint_hero_scenic_art(painter: &Painter, rect: Rect, game: &GameEntry, time: f64) {
    let t = time as f32;
    let cx = rect.left() + rect.width() * 0.72;
    let cy = rect.top() + rect.height() * 0.44;
    let scale = rect.height().min(rect.width() * 0.65);

    if game.title.contains("ASTRO") {
        // Huge glowing cosmic rings + floating geometric PS symbols in the background
        for i in 1..=4 {
            let fi = i as f32;
            let r = scale * (0.20 + fi * 0.12) + (t * 0.6 + fi).sin() * 6.0;
            painter.circle_stroke(
                Pos2::new(cx, cy),
                r,
                Stroke::new(1.5_f32, Color32::from_rgba_unmultiplied(110, 210, 255, (52 - i * 9) as u8)),
            );
        }
    } else if game.title.contains("Demon") {
        // Dramatic diagonal cathedral god-rays in warm gold
        for i in 0..5 {
            let fi = i as f32;
            let x0 = cx - 180.0 + fi * 95.0;
            let drift = (t * 0.25 + fi).sin() * 14.0;
            painter.line_segment(
                [Pos2::new(x0 + drift, rect.top()), Pos2::new(x0 - 140.0 + drift, rect.bottom())],
                Stroke::new(42.0_f32, Color32::from_rgba_unmultiplied(230, 178, 92, 11)),
            );
        }
    } else if game.title.contains("Ratchet") {
        // Swirling dimensional rift halo
        for i in 0..6 {
            let fi = i as f32;
            let ang = t * 0.22 + fi * 1.047;
            let off = Vec2::new(ang.cos() * 24.0, ang.sin() * 16.0);
            painter.circle_stroke(
                Pos2::new(cx, cy) + off,
                scale * (0.18 + fi * 0.065),
                Stroke::new(2.0_f32, Color32::from_rgba_unmultiplied(210, 110, 255, 24)),
            );
        }
    } else if game.title.contains("Returnal") {
        // Giant ominous eclipse on the upper-right sky
        let p = Pos2::new(cx, cy - 20.0);
        for i in (1..=5).rev() {
            let f = i as f32 / 5.0;
            painter.circle_filled(
                p,
                scale * 0.36 * f,
                Color32::from_rgba_unmultiplied(45, 215, 195, ((1.0 - f * 0.6) * 26.0) as u8),
            );
        }
        painter.circle_filled(p, scale * 0.19, Color32::from_rgba_unmultiplied(6, 12, 20, 210));
        painter.circle_stroke(
            p,
            scale * 0.195,
            Stroke::new(2.5_f32, Color32::from_rgba_unmultiplied(110, 250, 232, 130)),
        );
    } else if game.title.contains("Gran Turismo") {
        // High-speed crimson/white light-trails sweeping across the horizon
        for i in 0..6 {
            let fi = i as f32;
            let y = cy + 60.0 - fi * 28.0;
            let mut pts = Vec::with_capacity(28);
            for s in 0..=27 {
                let u = s as f32 / 27.0;
                let x = rect.left() + rect.width() * (0.35 + u * 0.65);
                let curve = -(u * u) * 95.0 + (u * 3.0 + t * 0.6).sin() * 6.0;
                pts.push(Pos2::new(x, y + curve));
            }
            let col = if i % 2 == 0 {
                Color32::from_rgba_unmultiplied(245, 58, 76, 36 - i * 4)
            } else {
                Color32::from_rgba_unmultiplied(255, 255, 255, 22 - i * 2)
            };
            painter.add(Shape::line(pts, Stroke::new(2.2_f32, col)));
        }
    }
}

/// Minimalist PS5 vector search icon (circle + diagonal stem).
pub fn paint_search_icon(painter: &Painter, center: Pos2, size: f32, color: Color32) {
    let r = size * 0.34;
    let c = center - Vec2::new(size * 0.08, size * 0.08);
    let stroke = Stroke::new(1.8_f32, color);
    painter.circle_stroke(c, r, stroke);
    painter.line_segment(
        [
            c + Vec2::new(r * 0.72, r * 0.72),
            center + Vec2::new(size * 0.42, size * 0.42),
        ],
        stroke,
    );
}

/// Minimalist PS5 vector settings gear icon.
pub fn paint_settings_icon(painter: &Painter, center: Pos2, size: f32, color: Color32) {
    let r_outer = size * 0.36;
    let r_inner = size * 0.16;
    let stroke = Stroke::new(1.8_f32, color);
    painter.circle_stroke(center, r_inner, stroke);
    painter.circle_stroke(center, r_outer * 0.82, Stroke::new(1.5_f32, color));
    for i in 0..8 {
        let ang = (i as f32) * std::f32::consts::FRAC_PI_4;
        let dir = Vec2::new(ang.cos(), ang.sin());
        painter.line_segment(
            [center + dir * (r_outer * 0.78), center + dir * (r_outer * 1.15)],
            Stroke::new(2.2_f32, color),
        );
    }
}

/// Minimalist PS5 circular profile avatar with green online status dot.
pub fn paint_avatar_icon(painter: &Painter, center: Pos2, radius: f32) {
    painter.circle_filled(center, radius, Color32::from_rgb(28, 92, 185));
    painter.circle_stroke(
        center,
        radius,
        Stroke::new(1.4_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 190)),
    );
    // Head + shoulders silhouette
    painter.circle_filled(center - Vec2::new(0.0, radius * 0.22), radius * 0.28, Color32::WHITE);
    painter.circle_filled(center + Vec2::new(0.0, radius * 0.42), radius * 0.42, Color32::from_rgba_unmultiplied(255, 255, 255, 230));
    // Re-mask bottom edge softly with border
    painter.circle_stroke(center, radius, Stroke::new(1.4_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 210)));
    // PS5 green online status dot
    let dot_pos = center + Vec2::new(radius * 0.72, radius * 0.72);
    painter.circle_filled(dot_pos, 4.6, Color32::from_rgb(10, 14, 24));
    painter.circle_filled(dot_pos, 3.2, Color32::from_rgb(76, 224, 128));
}

/// Crisp PlayStation controller symbol inside a dark circle (for bottom hint bar).
pub fn paint_ps_button_hint(
    painter: &Painter,
    center: Pos2,
    radius: f32,
    symbol: char,
) {
    painter.circle_filled(center, radius, Color32::from_rgba_unmultiplied(255, 255, 255, 24));
    painter.circle_stroke(
        center,
        radius,
        Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 80)),
    );
    let s = radius * 0.46;
    match symbol {
        'X' => {
            let st = Stroke::new(1.6_f32, Color32::from_rgb(138, 188, 255));
            painter.line_segment([center + Vec2::new(-s, -s), center + Vec2::new(s, s)], st);
            painter.line_segment([center + Vec2::new(s, -s), center + Vec2::new(-s, s)], st);
        }
        'O' => {
            painter.circle_stroke(center, s * 1.05, Stroke::new(1.6_f32, Color32::from_rgb(255, 118, 128)));
        }
        'T' => {
            let st = Stroke::new(1.6_f32, Color32::from_rgb(92, 225, 178));
            let pts = vec![
                center + Vec2::new(0.0, -s * 1.1),
                center + Vec2::new(s * 1.1, s * 0.85),
                center + Vec2::new(-s * 1.1, s * 0.85),
                center + Vec2::new(0.0, -s * 1.1),
            ];
            painter.add(Shape::line(pts, st));
        }
        _ => {
            painter.rect_stroke(
                Rect::from_center_size(center, Vec2::splat(s * 1.8)),
                CornerRadius::same(1),
                Stroke::new(1.6_f32, Color32::from_rgb(230, 145, 245)),
                StrokeKind::Inside,
            );
        }
    }
}
