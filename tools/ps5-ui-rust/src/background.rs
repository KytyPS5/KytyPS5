use egui::{Color32, Mesh, Painter, Pos2, Rect, Shape, Stroke, Vec2};

pub fn lerp_u8(a: u8, b: u8, t: f32) -> u8 {
    (a as f32 + (b as f32 - a as f32) * t.clamp(0.0, 1.0)).round() as u8
}

pub fn lerp_color(a: Color32, b: Color32, t: f32) -> Color32 {
    Color32::from_rgba_unmultiplied(
        lerp_u8(a.r(), b.r(), t),
        lerp_u8(a.g(), b.g(), t),
        lerp_u8(a.b(), b.b(), t),
        lerp_u8(a.a(), b.a(), t),
    )
}

pub fn paint_vertical_gradient(painter: &Painter, rect: Rect, top: Color32, bottom: Color32) {
    let mut mesh = Mesh::default();
    let idx = mesh.vertices.len() as u32;
    mesh.colored_vertex(rect.left_top(), top);
    mesh.colored_vertex(rect.right_top(), top);
    mesh.colored_vertex(rect.right_bottom(), bottom);
    mesh.colored_vertex(rect.left_bottom(), bottom);
    mesh.add_triangle(idx, idx + 1, idx + 2);
    mesh.add_triangle(idx, idx + 2, idx + 3);
    painter.add(Shape::mesh(mesh));
}

pub fn paint_diagonal_gradient(
    painter: &Painter,
    rect: Rect,
    tl: Color32,
    tr: Color32,
    br: Color32,
    bl: Color32,
) {
    let mut mesh = Mesh::default();
    let idx = mesh.vertices.len() as u32;
    mesh.colored_vertex(rect.left_top(), tl);
    mesh.colored_vertex(rect.right_top(), tr);
    mesh.colored_vertex(rect.right_bottom(), br);
    mesh.colored_vertex(rect.left_bottom(), bl);
    mesh.add_triangle(idx, idx + 1, idx + 2);
    mesh.add_triangle(idx, idx + 2, idx + 3);
    painter.add(Shape::mesh(mesh));
}

pub fn paint_ps5_background(
    painter: &Painter,
    rect: Rect,
    time: f64,
    primary: Color32,
    secondary: Color32,
) {
    let t = time as f32;

    // 1. Deep PS5 midnight base gradient
    let top_left = Color32::from_rgb(
        (secondary.r() as f32 * 0.65).min(255.0) as u8,
        (secondary.g() as f32 * 0.65).min(255.0) as u8,
        (secondary.b() as f32 * 0.85 + 10.0).min(255.0) as u8,
    );
    let top_right = Color32::from_rgb(
        (primary.r() as f32 * 0.34).min(255.0) as u8,
        (primary.g() as f32 * 0.34).min(255.0) as u8,
        (primary.b() as f32 * 0.42 + 12.0).min(255.0) as u8,
    );
    let bottom_left = Color32::from_rgb(5, 7, 13);
    let bottom_right = Color32::from_rgb(8, 12, 22);

    paint_diagonal_gradient(painter, rect, top_left, top_right, bottom_right, bottom_left);

    // 2. Soft ambient radial glows (PS5 upper-right hero aura + warm golden undertone)
    let glow1_center = Pos2::new(
        rect.left() + rect.width() * (0.72 + 0.04 * (t * 0.28).sin()),
        rect.top() + rect.height() * (0.34 + 0.03 * (t * 0.21).cos()),
    );
    for layer in (1..=7).rev() {
        let frac = layer as f32 / 7.0;
        let radius = rect.width().max(rect.height()) * 0.48 * frac;
        let alpha = ((1.0 - frac).powf(1.6) * 28.0) as u8;
        painter.circle_filled(
            glow1_center,
            radius,
            Color32::from_rgba_unmultiplied(primary.r(), primary.g(), primary.b(), alpha),
        );
    }

    let glow2_center = Pos2::new(
        rect.left() + rect.width() * (0.22 + 0.03 * (t * 0.19).cos()),
        rect.top() + rect.height() * (0.78 + 0.02 * (t * 0.24).sin()),
    );
    for layer in (1..=5).rev() {
        let frac = layer as f32 / 5.0;
        let radius = rect.width().max(rect.height()) * 0.36 * frac;
        let alpha = ((1.0 - frac).powf(1.7) * 18.0) as u8;
        painter.circle_filled(
            glow2_center,
            radius,
            Color32::from_rgba_unmultiplied(235, 190, 110, alpha),
        );
    }

    // 3. Flowing translucent PS5 light ribbons
    let ribbon_count = 4;
    let segments = 48;
    for r in 0..ribbon_count {
        let rf = r as f32;
        let mut pts = Vec::with_capacity(segments + 1);
        let y_base = rect.top() + rect.height() * (0.44 + rf * 0.075);
        for s in 0..=segments {
            let u = s as f32 / segments as f32;
            let x = rect.left() + u * rect.width();
            let wave1 = (u * 4.2 + t * (0.32 + rf * 0.05) + rf * 1.1).sin() * (28.0 + rf * 8.0);
            let wave2 = (u * 8.5 - t * 0.22 + rf * 0.7).cos() * 12.0;
            pts.push(Pos2::new(x, y_base + wave1 + wave2));
        }
        let ribbon_col = if r % 2 == 0 {
            Color32::from_rgba_unmultiplied(primary.r(), primary.g(), primary.b(), 26 - r * 3)
        } else {
            Color32::from_rgba_unmultiplied(235, 205, 145, 18 - r * 2)
        };
        painter.add(Shape::line(pts, Stroke::new(2.0 + rf * 1.6, ribbon_col)));
    }

    // 4. PS5 signature drifting golden/blue stardust particles
    let particle_count = 46;
    for i in 0..particle_count {
        let fi = i as f32;
        let seed_x = ((fi * 73.137).sin() * 0.5 + 0.5).clamp(0.02, 0.98);
        let seed_y = (fi * 19.731).sin() * 0.5 + 0.5;
        let speed = 0.012 + (i % 5) as f32 * 0.004;
        let y_norm = (seed_y - t * speed).rem_euclid(1.0);
        let drift_x = (t * 0.35 + fi * 1.7).sin() * 0.018;

        let pos = Pos2::new(
            rect.left() + (seed_x + drift_x).clamp(0.0, 1.0) * rect.width(),
            rect.top() + y_norm * rect.height(),
        );
        let shimmer = 0.45 + 0.55 * (t * (1.2 + (i % 4) as f32 * 0.35) + fi).sin();
        let radius = 1.2 + (i % 4) as f32 * 0.85;

        let (pr, pg, pb) = if i % 3 == 0 {
            (255, 224, 152) // PS5 gold stardust
        } else if i % 3 == 1 {
            (primary.r().max(110), primary.g().max(150), 255)
        } else {
            (225, 238, 255)
        };

        let alpha = (shimmer * 95.0) as u8;
        painter.circle_filled(pos, radius * 2.2, Color32::from_rgba_unmultiplied(pr, pg, pb, alpha / 4));
        painter.circle_filled(pos, radius, Color32::from_rgba_unmultiplied(pr, pg, pb, alpha));
    }

    // 5. Subtle cinematic bottom vignette so lower text & Play button pop
    let vignette_rect = Rect::from_min_max(
        Pos2::new(rect.left(), rect.top() + rect.height() * 0.42),
        rect.max,
    );
    paint_vertical_gradient(
        painter,
        vignette_rect,
        Color32::from_rgba_unmultiplied(6, 9, 16, 0),
        Color32::from_rgba_unmultiplied(4, 6, 12, 215),
    );
}

pub fn paint_boot_intro(painter: &Painter, rect: Rect, elapsed: f32) {
    // Deep black-to-royal-blue PS5 boot backdrop
    paint_vertical_gradient(
        painter,
        rect,
        Color32::from_rgb(3, 7, 18),
        Color32::from_rgb(8, 18, 42),
    );

    let center = rect.center();
    let progress = (elapsed / 2.2).clamp(0.0, 1.0);
    let fade_out = if elapsed > 2.2 {
        (1.0 - (elapsed - 2.2) / 0.6).clamp(0.0, 1.0)
    } else {
        1.0
    };

    // Expanding golden-blue halo
    for i in (1..=6).rev() {
        let f = i as f32 / 6.0;
        let r = (90.0 + progress * 240.0) * f;
        let a = ((1.0 - f) * 45.0 * fade_out) as u8;
        painter.circle_filled(center, r, Color32::from_rgba_unmultiplied(0, 130, 255, a));
    }

    // Rising PS5 golden particles
    for i in 0..36 {
        let fi = i as f32;
        let angle = fi * 2.39996; // golden angle
        let dist = (30.0 + fi * 9.5) * (0.35 + 0.65 * progress);
        let p = center
            + Vec2::new(
                angle.cos() * dist * 1.4,
                angle.sin() * dist * 0.85 - elapsed * 24.0,
            );
        let a = (150.0 * fade_out * (0.5 + 0.5 * (elapsed * 3.0 + fi).sin())) as u8;
        let col = if i % 2 == 0 {
            Color32::from_rgba_unmultiplied(255, 215, 130, a)
        } else {
            Color32::from_rgba_unmultiplied(120, 195, 255, a)
        };
        painter.circle_filled(p, 2.0 + (i % 3) as f32, col);
    }

    // PlayStation-style geometric symbols (△ ○ ✕ □) + PS5 • KYTY title
    let text_alpha = ((elapsed / 0.8).clamp(0.0, 1.0) * 255.0 * fade_out) as u8;
    painter.text(
        center - Vec2::new(0.0, 22.0),
        egui::Align2::CENTER_CENTER,
        "△   ○   ✕   □",
        egui::FontId::proportional(28.0),
        Color32::from_rgba_unmultiplied(150, 205, 255, text_alpha),
    );
    painter.text(
        center + Vec2::new(0.0, 24.0),
        egui::Align2::CENTER_CENTER,
        "PS5  •  KYTY SHELL",
        egui::FontId::proportional(34.0),
        Color32::from_rgba_unmultiplied(245, 248, 255, text_alpha),
    );
    painter.text(
        Pos2::new(center.x, rect.bottom() - 56.0),
        egui::Align2::CENTER_CENTER,
        "Atlamak için herhangi bir tuşa bas",
        egui::FontId::proportional(13.0),
        Color32::from_rgba_unmultiplied(180, 195, 220, (text_alpha as f32 * 0.7) as u8),
    );
}
