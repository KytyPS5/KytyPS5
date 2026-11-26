use crate::artwork::{
    font_bold, font_light, font_semilight, paint_avatar_icon, paint_game_tile_artwork,
    paint_hero_scenic_art, paint_ps_button_hint, paint_rounded_card_base, paint_search_icon,
    paint_settings_icon,
};
use crate::background::{
    lerp_color, paint_boot_intro, paint_ps5_background, paint_vertical_gradient,
};
use crate::config::{AppConfig, PssrUpscaler};
use crate::gamepad::{GamepadManager, NavEvent};
use crate::games::{build_game_list, default_media_entries, GameEntry, GameKind, MediaEntry};
use crate::launcher::{launch_game, open_url_or_path, RunningGame};
use chrono::Local;
use egui::{
    Align, Align2, Color32, Context, CornerRadius, Id, Key, Layout, Pos2, Rect, RichText, Sense,
    Stroke, StrokeKind, Vec2, ViewportCommand,
};
use std::path::PathBuf;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TopTab {
    Games,
    Media,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum OverlayScreen {
    None,
    ControlCenter,
    Settings,
    GameOptions,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SettingsCategory {
    System,
    Display,
    Ps5Pro,
    Controller,
}

struct Toast {
    title: String,
    body: String,
    created_at: Instant,
    is_error: bool,
}

pub struct Ps5ShellApp {
    config: AppConfig,
    games: Vec<GameEntry>,
    media: Vec<MediaEntry>,
    active_tab: TopTab,
    selected_game: usize,
    selected_media: usize,
    ribbon_scroll_x: f32,
    tile_scales: Vec<f32>,
    media_scales: Vec<f32>,

    overlay: OverlayScreen,
    settings_cat: SettingsCategory,
    search_open: bool,
    search_query: String,

    bg_primary: Color32,
    bg_secondary: Color32,
    prev_bg_game: Option<usize>,
    current_bg_game: usize,
    bg_fade: f32,

    boot_started_at: Instant,
    in_boot_intro: bool,
    is_window_fullscreen: bool,

    gamepad: GamepadManager,
    running_game: Option<RunningGame>,
    toasts: Vec<Toast>,
}

impl Ps5ShellApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        egui_extras::install_image_loaders(&cc.egui_ctx);
        configure_ps5_style(&cc.egui_ctx);

        let config = AppConfig::load();
        let games = build_game_list(&config.games_dir, config.show_demo_games);
        let media = default_media_entries();
        let in_boot_intro = !config.skip_boot_intro;

        let tile_scales = (0..games.len())
            .map(|i| if i == 0 { 1.0 } else { 0.0 })
            .collect();
        let media_scales = (0..media.len())
            .map(|i| if i == 0 { 1.0 } else { 0.0 })
            .collect();

        let (bg_primary, bg_secondary) = games
            .first()
            .map(|g| (g.accent_primary, g.accent_secondary))
            .unwrap_or((Color32::from_rgb(0, 112, 209), Color32::from_rgb(8, 22, 54)));

        Self {
            config,
            games,
            media,
            active_tab: TopTab::Games,
            selected_game: 0,
            selected_media: 0,
            ribbon_scroll_x: 0.0,
            tile_scales,
            media_scales,
            overlay: OverlayScreen::None,
            settings_cat: SettingsCategory::System,
            search_open: false,
            search_query: String::new(),
            bg_primary,
            bg_secondary,
            prev_bg_game: None,
            current_bg_game: 0,
            bg_fade: 1.0,
            boot_started_at: Instant::now(),
            in_boot_intro,
            is_window_fullscreen: false,
            gamepad: GamepadManager::new(),
            running_game: None,
            toasts: Vec::new(),
        }
    }

    fn push_toast(&mut self, title: impl Into<String>, body: impl Into<String>, is_error: bool) {
        self.toasts.push(Toast {
            title: title.into(),
            body: body.into(),
            created_at: Instant::now(),
            is_error,
        });
        if self.toasts.len() > 4 {
            self.toasts.remove(0);
        }
    }

    fn refresh_games(&mut self) {
        self.games = build_game_list(&self.config.games_dir, self.config.show_demo_games);
        if self.selected_game >= self.games.len() {
            self.selected_game = self.games.len().saturating_sub(1);
        }
        self.tile_scales = (0..self.games.len())
            .map(|i| if i == self.selected_game { 1.0 } else { 0.0 })
            .collect();
    }

    fn save_config(&mut self) {
        if let Err(err) = self.config.save() {
            self.push_toast("Ayarlar Kaydedilemedi", err, true);
        }
    }

    fn pick_games_folder(&mut self) {
        if let Some(folder) = rfd::FileDialog::new()
            .set_title("PS5 Oyun Klasörünü Seç")
            .pick_folder()
        {
            self.config.games_dir = folder.display().to_string();
            self.save_config();
            self.refresh_games();
            let count = self
                .games
                .iter()
                .filter(|g| matches!(g.kind, GameKind::Ps5Folder | GameKind::Elf | GameKind::ZArchive))
                .count();
            self.push_toast(
                "Kütüphane Güncellendi",
                format!("{count} PS5 oyunu/dosyası bulundu."),
                false,
            );
        }
    }

    fn pick_single_game_and_launch(&mut self) {
        if let Some(file) = rfd::FileDialog::new()
            .set_title("PS5 ELF veya ZArchive (.zar) Dosyası Seç")
            .add_filter("PS5 Oyun / Homebrew", &["elf", "zar", "bin"])
            .pick_file()
        {
            let title = file
                .file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("PS5 Oyunu")
                .to_string();
            self.start_game_path(&title, &file);
        }
    }

    fn pick_emulator_exe(&mut self) {
        if let Some(file) = rfd::FileDialog::new()
            .set_title("kyty_emulator.exe Dosyasını Seç")
            .add_filter("Executable", &["exe"])
            .pick_file()
        {
            self.config.emulator_path = file.display().to_string();
            self.save_config();
            self.push_toast("Emülatör Yolu Güncellendi", file.display().to_string(), false);
        }
    }

    fn start_game_path(&mut self, title: &str, path: &PathBuf) {
        match launch_game(&self.config, title, path) {
            Ok(running) => {
                self.push_toast(
                    "KytyPS5 Başlatılıyor",
                    format!("{title} yükleniyor..."),
                    false,
                );
                self.running_game = Some(running);
            }
            Err(err) => {
                self.push_toast("Başlatma Hatası", err, true);
            }
        }
    }

    fn activate_selected_game(&mut self) {
        let Some(game) = self.games.get(self.selected_game).cloned() else {
            return;
        };
        match game.kind {
            GameKind::Ps5Folder | GameKind::Elf | GameKind::ZArchive => {
                if let Some(path) = game.path {
                    self.start_game_path(&game.title, &path);
                }
            }
            GameKind::Demo => {
                self.push_toast(
                    "Önizleme Kartı",
                    "Gerçek oyun başlatmak için '+' kartından PS5 oyun klasörünü seç veya '⋯' menüsünden ELF/ZAR çalıştır.",
                    false,
                );
            }
            GameKind::AddGameTile => self.pick_games_folder(),
            GameKind::LibraryTile => {
                self.overlay = OverlayScreen::Settings;
                self.settings_cat = SettingsCategory::System;
            }
        }
    }

    fn activate_selected_media(&mut self) {
        if let Some(item) = self.media.get(self.selected_media).cloned() {
            match open_url_or_path(&item.url) {
                Ok(()) => self.push_toast("Medya Açılıyor", item.title, false),
                Err(e) => self.push_toast("Hata", e, true),
            }
        }
    }

    fn poll_running_game(&mut self) {
        if let Some(running) = self.running_game.as_mut() {
            if let Ok(Some(status)) = running.child.try_wait() {
                let title = running.title.clone();
                self.running_game = None;
                if status.success() {
                    self.push_toast("Oyun Kapatıldı", format!("{title} sonlandı."), false);
                } else {
                    self.push_toast(
                        "KytyPS5 Çıkış Kodu",
                        format!("{title} kapandı ({status})."),
                        true,
                    );
                }
            }
        }
    }

    fn handle_navigation(&mut self, ctx: &Context) {
        if ctx.input(|i| i.key_pressed(Key::F11)) {
            self.is_window_fullscreen = !self.is_window_fullscreen;
            ctx.send_viewport_cmd(ViewportCommand::Fullscreen(self.is_window_fullscreen));
        }

        let mut events = self.gamepad.poll();

        if self.in_boot_intro {
            let any_input = !events.is_empty()
                || ctx.input(|i| {
                    i.pointer.any_click()
                        || i.key_pressed(Key::Enter)
                        || i.key_pressed(Key::Space)
                        || i.key_pressed(Key::Escape)
                });
            if any_input || self.boot_started_at.elapsed() > Duration::from_millis(2600) {
                self.in_boot_intro = false;
            }
            return;
        }

        let typing = ctx.wants_keyboard_input();
        if !typing {
            ctx.input(|i| {
                if i.key_pressed(Key::ArrowLeft) || i.key_pressed(Key::A) {
                    events.push(NavEvent::Left);
                }
                if i.key_pressed(Key::ArrowRight) || i.key_pressed(Key::D) {
                    events.push(NavEvent::Right);
                }
                if i.key_pressed(Key::Enter) || i.key_pressed(Key::Space) {
                    events.push(NavEvent::Confirm);
                }
                if i.key_pressed(Key::Escape) || i.key_pressed(Key::Backspace) {
                    events.push(NavEvent::Back);
                }
                if i.key_pressed(Key::Q) {
                    events.push(NavEvent::TabPrev);
                }
                if i.key_pressed(Key::E) {
                    events.push(NavEvent::TabNext);
                }
                if i.key_pressed(Key::Tab) || i.key_pressed(Key::C) {
                    events.push(NavEvent::ControlCenter);
                }
            });
        }

        for ev in events {
            match ev {
                NavEvent::ControlCenter => {
                    self.overlay = if self.overlay == OverlayScreen::ControlCenter {
                        OverlayScreen::None
                    } else {
                        OverlayScreen::ControlCenter
                    };
                }
                NavEvent::Back => {
                    if self.search_open {
                        self.search_open = false;
                        self.search_query.clear();
                    } else if self.overlay != OverlayScreen::None {
                        self.overlay = OverlayScreen::None;
                    }
                }
                NavEvent::TabPrev if self.overlay == OverlayScreen::None => {
                    self.active_tab = TopTab::Games;
                }
                NavEvent::TabNext if self.overlay == OverlayScreen::None => {
                    self.active_tab = TopTab::Media;
                }
                NavEvent::Left if self.overlay == OverlayScreen::None => match self.active_tab {
                    TopTab::Games if self.selected_game > 0 => self.selected_game -= 1,
                    TopTab::Media if self.selected_media > 0 => self.selected_media -= 1,
                    _ => {}
                },
                NavEvent::Right if self.overlay == OverlayScreen::None => match self.active_tab {
                    TopTab::Games if self.selected_game + 1 < self.games.len() => {
                        self.selected_game += 1;
                    }
                    TopTab::Media if self.selected_media + 1 < self.media.len() => {
                        self.selected_media += 1;
                    }
                    _ => {}
                },
                NavEvent::Confirm if self.overlay == OverlayScreen::None => match self.active_tab {
                    TopTab::Games => self.activate_selected_game(),
                    TopTab::Media => self.activate_selected_media(),
                },
                _ => {}
            }
        }
    }

    fn current_target_colors(&self) -> (Color32, Color32) {
        match self.active_tab {
            TopTab::Games => self
                .games
                .get(self.selected_game)
                .map(|g| (g.accent_primary, g.accent_secondary))
                .unwrap_or((Color32::from_rgb(0, 112, 209), Color32::from_rgb(8, 22, 54))),
            TopTab::Media => self
                .media
                .get(self.selected_media)
                .map(|m| (m.accent_primary, m.accent_secondary))
                .unwrap_or((Color32::from_rgb(0, 112, 209), Color32::from_rgb(8, 22, 54))),
        }
    }

    fn draw_top_bar(&mut self, ui: &mut egui::Ui, rect: Rect) {
        let painter = ui.painter();
        let top_y = rect.top() + 36.0;

        // Left: Minimalist PS5 Light-font tabs (no ugly underline, pure crisp white vs dimmed)
        let mut tab_x = rect.left() + 56.0;
        for (tab, label) in [(TopTab::Games, "Oyunlar"), (TopTab::Media, "Medya")] {
            let active = self.active_tab == tab;
            let color = if active {
                Color32::WHITE
            } else {
                Color32::from_rgba_unmultiplied(220, 228, 244, 115)
            };
            let galley = painter.layout_no_wrap(label.to_string(), font_light(23.0), color);
            let text_rect = Rect::from_min_size(Pos2::new(tab_x, top_y - galley.size().y * 0.5), galley.size());
            let resp = ui.interact(text_rect.expand(6.0), Id::new(("top_tab", label)), Sense::click());
            if resp.clicked() {
                self.active_tab = tab;
                self.overlay = OverlayScreen::None;
            }
            let draw_col = if resp.hovered() && !active {
                Color32::from_rgba_unmultiplied(240, 245, 255, 200)
            } else {
                color
            };
            painter.text(
                Pos2::new(tab_x, top_y),
                Align2::LEFT_CENTER,
                label,
                font_light(23.0),
                draw_col,
            );
            tab_x += galley.size().x + 34.0;
        }

        // Right: Authentic PS5 borderless icons -> Search, Settings, Profile Avatar, Light Clock
        let mut right_x = rect.right() - 56.0;

        // 1. Live Clock (Segoe UI Light)
        let now_str = Local::now().format("%H:%M").to_string();
        let clock_galley = painter.layout_no_wrap(now_str.clone(), font_light(23.0), Color32::WHITE);
        painter.text(
            Pos2::new(right_x, top_y),
            Align2::RIGHT_CENTER,
            now_str,
            font_light(23.0),
            Color32::from_rgba_unmultiplied(245, 248, 255, 240),
        );
        right_x -= clock_galley.size().x + 32.0;

        // 2. Circular User Avatar
        let avatar_center = Pos2::new(right_x - 16.0, top_y);
        let avatar_rect = Rect::from_center_size(avatar_center, Vec2::splat(36.0));
        let avatar_resp = ui.interact(avatar_rect, Id::new("ps5_avatar_btn"), Sense::click());
        if avatar_resp.hovered() {
            painter.circle_stroke(
                avatar_center,
                19.0,
                Stroke::new(1.5_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 185)),
            );
        }
        paint_avatar_icon(painter, avatar_center, 15.0);
        if avatar_resp.clicked() {
            self.overlay = OverlayScreen::Settings;
            self.settings_cat = SettingsCategory::System;
        }
        right_x -= 56.0;

        // 3. Settings Gear Vector Icon
        let gear_center = Pos2::new(right_x - 16.0, top_y);
        let gear_rect = Rect::from_center_size(gear_center, Vec2::splat(36.0));
        let gear_resp = ui.interact(gear_rect, Id::new("ps5_settings_btn"), Sense::click());
        if gear_resp.hovered() || self.overlay == OverlayScreen::Settings {
            painter.circle_filled(
                gear_center,
                18.0,
                Color32::from_rgba_unmultiplied(255, 255, 255, 32),
            );
        }
        paint_settings_icon(painter, gear_center, 22.0, Color32::from_rgba_unmultiplied(242, 246, 255, 230));
        if gear_resp.clicked() {
            self.overlay = if self.overlay == OverlayScreen::Settings {
                OverlayScreen::None
            } else {
                OverlayScreen::Settings
            };
        }
        right_x -= 50.0;

        // 4. Search Magnifier Vector Icon
        let search_center = Pos2::new(right_x - 16.0, top_y);
        let search_rect = Rect::from_center_size(search_center, Vec2::splat(36.0));
        let search_resp = ui.interact(search_rect, Id::new("ps5_search_btn"), Sense::click());
        if search_resp.hovered() || self.search_open {
            painter.circle_filled(
                search_center,
                18.0,
                Color32::from_rgba_unmultiplied(255, 255, 255, 32),
            );
        }
        paint_search_icon(painter, search_center, 20.0, Color32::from_rgba_unmultiplied(242, 246, 255, 230));
        if search_resp.clicked() {
            self.search_open = !self.search_open;
            if !self.search_open {
                self.search_query.clear();
            }
        }

        // Inline sleek glass search field if toggled open
        if self.search_open {
            let input_rect = Rect::from_min_size(Pos2::new(right_x - 230.0, top_y - 15.0), Vec2::new(190.0, 30.0));
            let mut search_ui = ui.new_child(egui::UiBuilder::new().max_rect(input_rect));
            search_ui.add_sized(
                Vec2::new(190.0, 30.0),
                egui::TextEdit::singleline(&mut self.search_query).hint_text("Oyun ara..."),
            );
        }
    }

    fn draw_games_ribbon_and_hero(&mut self, ui: &mut egui::Ui, rect: Rect, time: f64) {
        let query = self.search_query.trim().to_lowercase();
        let visible_indices: Vec<usize> = self
            .games
            .iter()
            .enumerate()
            .filter(|(_, g)| query.is_empty() || g.title.to_lowercase().contains(&query))
            .map(|(i, _)| i)
            .collect();

        if !visible_indices.is_empty() && !visible_indices.contains(&self.selected_game) {
            self.selected_game = visible_indices[0];
        }

        if self.tile_scales.len() != self.games.len() {
            self.tile_scales = vec![0.0; self.games.len()];
        }
        for (i, s) in self.tile_scales.iter_mut().enumerate() {
            let target = if i == self.selected_game { 1.0 } else { 0.0 };
            *s += (target - *s) * 0.18;
        }

        let selected_pos = visible_indices
            .iter()
            .position(|&idx| idx == self.selected_game)
            .unwrap_or(0);
        let target_scroll = (selected_pos as f32 * 104.0 - 110.0).max(0.0);
        self.ribbon_scroll_x += (target_scroll - self.ribbon_scroll_x) * 0.15;

        let ribbon_top = rect.top() + 84.0;
        let mut cursor_x = rect.left() + 56.0 - self.ribbon_scroll_x;
        let mut clicked_index: Option<usize> = None;
        let mut double_clicked = false;
        let mut selected_tile_rect: Option<Rect> = None;

        for &idx in &visible_indices {
            let game = &self.games[idx];
            let selected = idx == self.selected_game;
            let anim = self.tile_scales[idx];
            let size = 88.0 + anim * 36.0; // Smoothly scales 88px -> 124px
            let y_offset = (1.0 - anim) * 18.0;
            let tile_rect = Rect::from_min_size(Pos2::new(cursor_x, ribbon_top + y_offset), Vec2::splat(size));

            if selected {
                selected_tile_rect = Some(tile_rect);
            }

            if tile_rect.right() >= rect.left() - 24.0 && tile_rect.left() <= rect.right() + 24.0 {
                let resp = ui.interact(tile_rect, Id::new(("game_tile", idx)), Sense::click());
                if resp.clicked() {
                    if selected {
                        double_clicked = true;
                    } else {
                        clicked_index = Some(idx);
                    }
                }
                if resp.double_clicked() {
                    clicked_index = Some(idx);
                    double_clicked = true;
                }

                draw_ps5_game_tile(ui, tile_rect, game, selected, resp.hovered(), anim, time);
            }

            cursor_x += size + 16.0;
        }

        // Authentic PS5 subtle game name floating right beneath the focused tile
        if let (Some(tile_r), Some(game)) = (selected_tile_rect, self.games.get(self.selected_game)) {
            ui.painter().text(
                Pos2::new(tile_r.left() + 2.0, tile_r.bottom() + 18.0),
                Align2::LEFT_CENTER,
                &game.title,
                font_semilight(14.5),
                Color32::from_rgba_unmultiplied(245, 248, 255, 235),
            );
        }

        if let Some(idx) = clicked_index {
            self.selected_game = idx;
        }
        if double_clicked {
            self.activate_selected_game();
        }

        // Hero Section (Lower-Left) + PS5 Activity/Engine Status Card (Lower-Right)
        if let Some(game) = self.games.get(self.selected_game).cloned() {
            let painter = ui.painter();
            let hero_bottom = rect.bottom() - 78.0;
            let left_x = rect.left() + 56.0;

            // 1. Primary Play Pill + Circular "..." button (drawn custom, zero egui gray boxes!)
            let play_label = match game.kind {
                GameKind::AddGameTile => "Klasör Seç",
                GameKind::LibraryTile => "Ayarları Aç",
                GameKind::Demo => "Oyna",
                _ => "Oyna",
            };

            let play_rect = Rect::from_min_size(Pos2::new(left_x, hero_bottom - 48.0), Vec2::new(224.0, 48.0));
            let play_resp = ui.interact(play_rect, Id::new("ps5_play_pill"), Sense::click());
            let play_hover = play_resp.hovered();

            // Authentic PS5 translucent frosted pill with crisp white focus outline
            painter.rect_filled(
                play_rect,
                CornerRadius::same(24),
                if play_hover {
                    Color32::from_rgba_unmultiplied(255, 255, 255, 238)
                } else {
                    Color32::from_rgba_unmultiplied(255, 255, 255, 34)
                },
            );
            painter.rect_stroke(
                play_rect.expand(if play_hover { 3.0 } else { 0.0 }),
                CornerRadius::same(26),
                Stroke::new(
                    if play_hover { 2.0_f32 } else { 1.2_f32 },
                    Color32::from_rgba_unmultiplied(255, 255, 255, if play_hover { 245 } else { 165 }),
                ),
                StrokeKind::Outside,
            );
            painter.text(
                play_rect.center(),
                Align2::CENTER_CENTER,
                play_label,
                font_semilight(18.0),
                if play_hover {
                    Color32::from_rgb(12, 16, 26)
                } else {
                    Color32::WHITE
                },
            );
            if play_resp.clicked() {
                self.activate_selected_game();
            }

            // Circular "..." options button right beside Play pill
            let opts_center = Pos2::new(play_rect.right() + 36.0, play_rect.center().y);
            let opts_rect = Rect::from_center_size(opts_center, Vec2::splat(48.0));
            let opts_resp = ui.interact(opts_rect, Id::new("ps5_opts_circle"), Sense::click());
            let opts_hover = opts_resp.hovered();
            painter.circle_filled(
                opts_center,
                24.0,
                Color32::from_rgba_unmultiplied(255, 255, 255, if opts_hover { 56 } else { 28 }),
            );
            painter.circle_stroke(
                opts_center,
                24.0,
                Stroke::new(
                    1.2_f32,
                    Color32::from_rgba_unmultiplied(255, 255, 255, if opts_hover { 220 } else { 70 }),
                ),
            );
            for dx in [-6.5_f32, 0.0, 6.5] {
                painter.circle_filled(opts_center + Vec2::new(dx, 0.0), 2.1, Color32::WHITE);
            }
            if opts_resp.clicked() {
                self.overlay = OverlayScreen::GameOptions;
            }

            // 2. Subtitle above Play pill
            let sub_y = play_rect.top() - 26.0;
            painter.text(
                Pos2::new(left_x, sub_y),
                Align2::LEFT_BOTTOM,
                &game.subtitle,
                font_semilight(14.5),
                Color32::from_rgba_unmultiplied(218, 228, 244, 185),
            );

            // 3. Large Airy PS5 Title (Segoe UI Light 50px with soft shadow)
            let title_y = sub_y - 28.0;
            painter.text(
                Pos2::new(left_x + 1.5, title_y + 2.0),
                Align2::LEFT_BOTTOM,
                &game.title,
                font_light(50.0),
                Color32::from_rgba_unmultiplied(0, 0, 0, 140),
            );
            painter.text(
                Pos2::new(left_x, title_y),
                Align2::LEFT_BOTTOM,
                &game.title,
                font_light(50.0),
                Color32::WHITE,
            );

            // 4. Crisp PS5 Tag + Title ID row above Title
            let tag_y = title_y - 62.0;
            let tag_rect = Rect::from_min_size(Pos2::new(left_x, tag_y - 20.0), Vec2::new(44.0, 20.0));
            painter.rect_filled(tag_rect, CornerRadius::same(4), Color32::from_rgba_unmultiplied(255, 255, 255, 230));
            painter.text(
                tag_rect.center(),
                Align2::CENTER_CENTER,
                "PS5",
                font_bold(11.0),
                Color32::from_rgb(12, 16, 26),
            );
            let meta_line = if game.version.is_empty() {
                game.title_id.clone()
            } else {
                format!("{}   •   v{}", game.title_id, game.version)
            };
            painter.text(
                Pos2::new(tag_rect.right() + 12.0, tag_rect.center().y),
                Align2::LEFT_CENTER,
                meta_line,
                font_semilight(13.0),
                Color32::from_rgba_unmultiplied(220, 230, 245, 180),
            );

            // 5. Bottom-Right Authentic PS5 Frosted Status / Activity Card
            let card_w = 304.0;
            let card_h = 88.0;
            let card_rect = Rect::from_min_size(
                Pos2::new(rect.right() - 56.0 - card_w, hero_bottom - card_h),
                Vec2::new(card_w, card_h),
            );
            painter.rect_filled(
                card_rect,
                CornerRadius::same(16),
                Color32::from_rgba_unmultiplied(14, 20, 34, 175),
            );
            painter.rect_stroke(
                card_rect,
                CornerRadius::same(16),
                Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 36)),
                StrokeKind::Inside,
            );

            let emu_ok = PathBuf::from(&self.config.emulator_path).exists();
            let mode_label = if self.config.trinity_mode {
                format!("PS5 PRO • TRINITY ({})", self.config.pssr_upscaler.as_cli_arg())
            } else {
                "KYTYPS5 • VULKAN ENGINE".to_string()
            };
            painter.text(
                Pos2::new(card_rect.left() + 18.0, card_rect.top() + 22.0),
                Align2::LEFT_CENTER,
                mode_label,
                font_bold(11.0),
                Color32::from_rgb(138, 202, 255),
            );

            let res_str = format!(
                "{}×{}  •  {} Hz  •  {}",
                self.config.screen_width,
                self.config.screen_height,
                self.config.vblank_frequency,
                if self.config.shader_cache { "Shader Cache" } else { "Cache Kapalı" }
            );
            painter.text(
                Pos2::new(card_rect.left() + 18.0, card_rect.top() + 46.0),
                Align2::LEFT_CENTER,
                res_str,
                font_semilight(13.0),
                Color32::from_rgba_unmultiplied(238, 244, 255, 225),
            );

            let (dot_col, status_str) = if let Some(running) = &self.running_game {
                (Color32::from_rgb(80, 232, 152), format!("Çalışıyor: {}", running.title))
            } else if emu_ok {
                (Color32::from_rgb(80, 232, 152), "Emülatör bağlantısı hazır".to_string())
            } else {
                (Color32::from_rgb(245, 110, 110), "kyty_emulator.exe bulunamadı".to_string())
            };
            painter.circle_filled(Pos2::new(card_rect.left() + 23.0, card_rect.bottom() - 19.0), 4.0, dot_col);
            painter.text(
                Pos2::new(card_rect.left() + 34.0, card_rect.bottom() - 19.0),
                Align2::LEFT_CENTER,
                status_str,
                font_semilight(12.0),
                Color32::from_rgba_unmultiplied(195, 210, 234, 195),
            );
        }
    }

    fn draw_media_ribbon_and_hero(&mut self, ui: &mut egui::Ui, rect: Rect, time: f64) {
        if self.media_scales.len() != self.media.len() {
            self.media_scales = vec![0.0; self.media.len()];
        }
        for (i, s) in self.media_scales.iter_mut().enumerate() {
            let target = if i == self.selected_media { 1.0 } else { 0.0 };
            *s += (target - *s) * 0.18;
        }

        let ribbon_top = rect.top() + 84.0;
        let mut cursor_x = rect.left() + 56.0;
        let mut clicked_idx = None;
        let mut activate = false;

        for (idx, item) in self.media.iter().enumerate() {
            let selected = idx == self.selected_media;
            let anim = self.media_scales[idx];
            let size = 88.0 + anim * 36.0;
            let y_offset = (1.0 - anim) * 18.0;
            let tile_rect = Rect::from_min_size(Pos2::new(cursor_x, ribbon_top + y_offset), Vec2::splat(size));

            let resp = ui.interact(tile_rect, Id::new(("media_tile", idx)), Sense::click());
            if resp.clicked() {
                if selected {
                    activate = true;
                } else {
                    clicked_idx = Some(idx);
                }
            }
            if resp.double_clicked() {
                clicked_idx = Some(idx);
                activate = true;
            }

            draw_ps5_media_tile(ui, tile_rect, item, selected, resp.hovered(), anim, time);
            cursor_x += size + 16.0;
        }

        if let Some(idx) = clicked_idx {
            self.selected_media = idx;
        }
        if activate {
            self.activate_selected_media();
        }

        if let Some(item) = self.media.get(self.selected_media).cloned() {
            let painter = ui.painter();
            let hero_bottom = rect.bottom() - 78.0;
            let left_x = rect.left() + 56.0;

            let open_rect = Rect::from_min_size(Pos2::new(left_x, hero_bottom - 48.0), Vec2::new(200.0, 48.0));
            let open_resp = ui.interact(open_rect, Id::new("ps5_media_open"), Sense::click());
            let hover = open_resp.hovered();
            painter.rect_filled(
                open_rect,
                CornerRadius::same(24),
                if hover {
                    Color32::from_rgba_unmultiplied(255, 255, 255, 238)
                } else {
                    Color32::from_rgba_unmultiplied(255, 255, 255, 34)
                },
            );
            painter.rect_stroke(
                open_rect,
                CornerRadius::same(24),
                Stroke::new(1.4_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 190)),
                StrokeKind::Outside,
            );
            painter.text(
                open_rect.center(),
                Align2::CENTER_CENTER,
                "Aç",
                font_semilight(18.0),
                if hover { Color32::from_rgb(12, 16, 26) } else { Color32::WHITE },
            );
            if open_resp.clicked() {
                self.activate_selected_media();
            }

            painter.text(
                Pos2::new(left_x, open_rect.top() - 26.0),
                Align2::LEFT_BOTTOM,
                &item.subtitle,
                font_semilight(14.5),
                Color32::from_rgba_unmultiplied(218, 228, 244, 185),
            );
            painter.text(
                Pos2::new(left_x, open_rect.top() - 54.0),
                Align2::LEFT_BOTTOM,
                &item.title,
                font_light(50.0),
                Color32::WHITE,
            );
        }
    }

    fn draw_bottom_hint_bar(&mut self, ui: &mut egui::Ui, rect: Rect) {
        let painter = ui.painter();
        let y = rect.bottom() - 28.0;
        let mut x = rect.left() + 56.0;
        let label_col = Color32::from_rgba_unmultiplied(215, 226, 244, 170);

        // X (Confirm)
        paint_ps_button_hint(painter, Pos2::new(x + 9.0, y), 9.0, 'X');
        x += 24.0;
        let g1 = painter.layout_no_wrap("Seç / Oyna".into(), font_semilight(12.5), label_col);
        painter.text(Pos2::new(x, y), Align2::LEFT_CENTER, "Seç / Oyna", font_semilight(12.5), label_col);
        x += g1.size().x + 26.0;

        // O (Back)
        paint_ps_button_hint(painter, Pos2::new(x + 9.0, y), 9.0, 'O');
        x += 24.0;
        let g2 = painter.layout_no_wrap("Geri".into(), font_semilight(12.5), label_col);
        painter.text(Pos2::new(x, y), Align2::LEFT_CENTER, "Geri", font_semilight(12.5), label_col);
        x += g2.size().x + 26.0;

        // Clickable Control Center trigger
        let cc_text = "Tab / PS   Kontrol Merkezi";
        let g3 = painter.layout_no_wrap(cc_text.into(), font_semilight(12.5), Color32::from_rgb(150, 210, 255));
        let cc_rect = Rect::from_min_size(Pos2::new(x, y - 10.0), Vec2::new(g3.size().x + 8.0, 20.0));
        let cc_resp = ui.interact(cc_rect, Id::new("bottom_cc_btn"), Sense::click());
        painter.text(
            Pos2::new(x, y),
            Align2::LEFT_CENTER,
            cc_text,
            font_semilight(12.5),
            if cc_resp.hovered() { Color32::WHITE } else { Color32::from_rgb(150, 210, 255) },
        );
        if cc_resp.clicked() {
            self.overlay = if self.overlay == OverlayScreen::ControlCenter {
                OverlayScreen::None
            } else {
                OverlayScreen::ControlCenter
            };
        }

        // Right-side hint: F11 Fullscreen + connected gamepad
        let right_text = if let Some(gp) = self.gamepad.connected_controller() {
            format!("Kontrolcü: {}   •   F11 Tam Ekran", gp.chars().take(20).collect::<String>())
        } else {
            "←/→ Gezin   •   Q/E Sekme   •   F11 Tam Ekran".to_string()
        };
        painter.text(
            Pos2::new(rect.right() - 56.0, y),
            Align2::RIGHT_CENTER,
            right_text,
            font_semilight(12.0),
            Color32::from_rgba_unmultiplied(195, 210, 232, 145),
        );
    }

    fn draw_control_center(&mut self, ctx: &Context) {
        let screen = ctx.screen_rect();
        let painter = ctx.layer_painter(egui::LayerId::new(egui::Order::Middle, Id::new("cc_dim")));
        painter.rect_filled(screen, 0.0, Color32::from_rgba_unmultiplied(4, 7, 14, 175));

        egui::Area::new(Id::new("ps5_control_center"))
            .order(egui::Order::Foreground)
            .anchor(Align2::CENTER_BOTTOM, Vec2::new(0.0, -34.0))
            .show(ctx, |ui| {
                // Upper row: Floating PS5 Activity Cards
                ui.vertical_centered(|ui| {
                    ui.horizontal(|ui| {
                        ui.spacing_mut().item_spacing.x = 16.0;
                        if ps5_activity_card(
                            ui,
                            "PS5 PRO • TRINITY",
                            if self.config.trinity_mode { "Aktif" } else { "Kapalı" },
                            &format!("PSSR: {}", self.config.pssr_upscaler.as_cli_arg()),
                            self.config.trinity_mode,
                        ) {
                            self.config.trinity_mode = !self.config.trinity_mode;
                            self.save_config();
                        }
                        if ps5_activity_card(
                            ui,
                            "PERFORMANS PANELİ",
                            if self.config.perf_overlay { "Açık (F2)" } else { "Kapalı" },
                            "FPS ve Vulkan istatistikleri",
                            self.config.perf_overlay,
                        ) {
                            self.config.perf_overlay = !self.config.perf_overlay;
                            self.save_config();
                        }
                        if ps5_activity_card(
                            ui,
                            "SHADER ÖNBELLEĞİ",
                            if self.config.shader_cache { "Açık" } else { "Kapalı" },
                            "Daha hızlı oyun açılışı",
                            self.config.shader_cache,
                        ) {
                            self.config.shader_cache = !self.config.shader_cache;
                            self.save_config();
                        }
                        if ps5_activity_card(
                            ui,
                            "EKRAN MODU",
                            if self.config.fullscreen { "Tam Ekran" } else { "Pencere" },
                            &format!("{}×{} • {} Hz", self.config.screen_width, self.config.screen_height, self.config.vblank_frequency),
                            self.config.fullscreen,
                        ) {
                            self.config.fullscreen = !self.config.fullscreen;
                            self.save_config();
                        }
                    });

                    ui.add_space(22.0);

                    // Bottom row: Authentic PS5 Circular Icon Dock
                    egui::Frame::new()
                        .fill(Color32::from_rgba_unmultiplied(16, 22, 36, 232))
                        .stroke(Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 34)))
                        .corner_radius(CornerRadius::same(38))
                        .inner_margin(egui::Margin::symmetric(26, 14))
                        .show(ui, |ui| {
                            ui.horizontal(|ui| {
                                ui.spacing_mut().item_spacing.x = 20.0;
                                if dock_circle_btn(ui, "⌂", "Ana Ekran", false) {
                                    self.overlay = OverlayScreen::None;
                                }
                                if dock_circle_btn(ui, "＋", "Klasör", false) {
                                    self.overlay = OverlayScreen::None;
                                    self.pick_games_folder();
                                }
                                if dock_circle_btn(ui, "▶", "ELF/ZAR", false) {
                                    self.overlay = OverlayScreen::None;
                                    self.pick_single_game_and_launch();
                                }
                                if dock_circle_btn(ui, "⚙", "Ayarlar", false) {
                                    self.overlay = OverlayScreen::Settings;
                                }
                                if dock_circle_btn(ui, "◈", "Kyty UI", false) {
                                    let launcher_exe = PathBuf::from(&self.config.emulator_path)
                                        .parent()
                                        .map(|p| p.join("kyty_launcher.exe"));
                                    if let Some(path) = launcher_exe.filter(|p| p.exists()) {
                                        let _ = open_url_or_path(&path.display().to_string());
                                        self.push_toast("Kyty Launcher", "Resmi arayüz açıldı.", false);
                                    } else {
                                        self.push_toast("Bulunamadı", "kyty_launcher.exe bulunamadı.", true);
                                    }
                                }
                                if let Some(running) = self.running_game.as_mut() {
                                    if dock_circle_btn(ui, "■", "Durdur", true) {
                                        let _ = running.child.kill();
                                        self.running_game = None;
                                        self.push_toast("Durduruldu", "Emülatör kapatıldı.", false);
                                    }
                                }
                                if dock_circle_btn(ui, "⏻", "Güç", false) {
                                    ctx.send_viewport_cmd(ViewportCommand::Close);
                                }
                            });
                        });
                });
            });
    }

    fn draw_game_options_modal(&mut self, ctx: &Context) {
        let Some(game) = self.games.get(self.selected_game).cloned() else {
            self.overlay = OverlayScreen::None;
            return;
        };

        let screen = ctx.screen_rect();
        let painter = ctx.layer_painter(egui::LayerId::new(egui::Order::Middle, Id::new("opts_dim")));
        painter.rect_filled(screen, 0.0, Color32::from_rgba_unmultiplied(4, 7, 14, 165));

        egui::Area::new(Id::new("ps5_game_options"))
            .order(egui::Order::Foreground)
            .anchor(Align2::CENTER_CENTER, Vec2::ZERO)
            .show(ctx, |ui| {
                egui::Frame::new()
                    .fill(Color32::from_rgba_unmultiplied(16, 22, 36, 246))
                    .stroke(Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 38)))
                    .corner_radius(CornerRadius::same(18))
                    .inner_margin(egui::Margin::same(24))
                    .show(ui, |ui| {
                        ui.set_min_width(360.0);
                        ui.label(RichText::new(&game.title).font(font_light(24.0)).color(Color32::WHITE));
                        ui.label(
                            RichText::new(format!("{} • {}", game.badge_text, game.title_id))
                                .font(font_semilight(12.5))
                                .color(Color32::from_rgb(160, 180, 212)),
                        );
                        ui.add_space(16.0);

                        if modal_pill_btn(ui, "Oyunu Başlat") {
                            self.overlay = OverlayScreen::None;
                            self.activate_selected_game();
                        }
                        ui.add_space(8.0);
                        if modal_pill_btn(ui, "Tekil ELF / ZAR Dosyası Çalıştır") {
                            self.overlay = OverlayScreen::None;
                            self.pick_single_game_and_launch();
                        }
                        ui.add_space(8.0);
                        if let Some(path) = &game.path {
                            if modal_pill_btn(ui, "Oyun Klasörünü Aç") {
                                let folder = if path.is_dir() {
                                    path.clone()
                                } else {
                                    path.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| path.clone())
                                };
                                let _ = open_url_or_path(&folder.display().to_string());
                                self.overlay = OverlayScreen::None;
                            }
                            ui.add_space(8.0);
                        }
                        if modal_pill_btn(ui, "Emülatör Ayarları") {
                            self.overlay = OverlayScreen::Settings;
                        }
                        ui.add_space(10.0);
                        if modal_pill_btn(ui, "Kapat (Esc)") {
                            self.overlay = OverlayScreen::None;
                        }
                    });
            });
    }

    fn draw_settings_screen(&mut self, ctx: &Context) {
        let screen = ctx.screen_rect();
        let painter = ctx.layer_painter(egui::LayerId::new(egui::Order::Middle, Id::new("settings_dim")));
        painter.rect_filled(screen, 0.0, Color32::from_rgba_unmultiplied(5, 9, 18, 232));

        egui::Area::new(Id::new("ps5_settings_panel"))
            .order(egui::Order::Foreground)
            .anchor(Align2::CENTER_CENTER, Vec2::ZERO)
            .show(ctx, |ui| {
                let panel_w = (screen.width() - 96.0).clamp(680.0, 980.0);
                let panel_h = (screen.height() - 108.0).clamp(440.0, 620.0);

                egui::Frame::new()
                    .fill(Color32::from_rgba_unmultiplied(14, 19, 32, 248))
                    .stroke(Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 30)))
                    .corner_radius(CornerRadius::same(22))
                    .inner_margin(egui::Margin::same(28))
                    .show(ui, |ui| {
                        ui.set_min_size(Vec2::new(panel_w, panel_h));
                        ui.set_max_size(Vec2::new(panel_w, panel_h));

                        ui.horizontal(|ui| {
                            ui.label(RichText::new("Ayarlar").font(font_light(30.0)).color(Color32::WHITE));
                            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                if ui.button("Kapat (Esc)").clicked() {
                                    self.save_config();
                                    self.overlay = OverlayScreen::None;
                                }
                            });
                        });
                        ui.add_space(14.0);
                        ui.separator();
                        ui.add_space(14.0);

                        let mut changed = false;

                        ui.columns(2, |cols| {
                            cols[0].set_max_width(235.0);
                            let cats = [
                                (SettingsCategory::System, "Sistem ve Emülatör"),
                                (SettingsCategory::Display, "Ekran ve Video"),
                                (SettingsCategory::Ps5Pro, "PS5 Pro ve PSSR"),
                                (SettingsCategory::Controller, "DualSense Aksesuar"),
                            ];
                            for (cat, label) in cats {
                                let active = self.settings_cat == cat;
                                let btn = egui::Button::new(
                                    RichText::new(label).font(font_semilight(15.0)).color(if active {
                                        Color32::from_rgb(12, 16, 26)
                                    } else {
                                        Color32::WHITE
                                    }),
                                )
                                .fill(if active {
                                    Color32::WHITE
                                } else {
                                    Color32::from_rgba_unmultiplied(255, 255, 255, 12)
                                })
                                .corner_radius(CornerRadius::same(12))
                                .min_size(Vec2::new(215.0, 44.0));
                                if cols[0].add(btn).clicked() {
                                    self.settings_cat = cat;
                                }
                                cols[0].add_space(6.0);
                            }

                            egui::ScrollArea::vertical().show(&mut cols[1], |ui| {
                                ui.spacing_mut().item_spacing.y = 10.0;
                                match self.settings_cat {
                                    SettingsCategory::System => {
                                        ui.label(RichText::new("KytyPS5 Emülatör Yolu").font(font_semilight(14.0)).color(Color32::WHITE));
                                        ui.horizontal(|ui| {
                                            changed |= ui
                                                .add_sized(
                                                    Vec2::new(340.0, 28.0),
                                                    egui::TextEdit::singleline(&mut self.config.emulator_path),
                                                )
                                                .changed();
                                            if ui.button("Gözat").clicked() {
                                                self.pick_emulator_exe();
                                            }
                                        });

                                        ui.add_space(6.0);
                                        ui.label(RichText::new("PS5 Oyunlar Klasörü").font(font_semilight(14.0)).color(Color32::WHITE));
                                        ui.horizontal(|ui| {
                                            changed |= ui
                                                .add_sized(
                                                    Vec2::new(340.0, 28.0),
                                                    egui::TextEdit::singleline(&mut self.config.games_dir)
                                                        .hint_text("Oyun klasörünü seç..."),
                                                )
                                                .changed();
                                            if ui.button("Seç").clicked() {
                                                self.pick_games_folder();
                                            }
                                            if ui.button("Tara").clicked() {
                                                self.refresh_games();
                                                self.push_toast("Tarandı", "Oyun listesi yenilendi.", false);
                                            }
                                        });

                                        ui.add_space(6.0);
                                        ui.horizontal(|ui| {
                                            ui.label("Profil Adı:");
                                            changed |= ui
                                                .add_sized(
                                                    Vec2::new(180.0, 26.0),
                                                    egui::TextEdit::singleline(&mut self.config.user_name),
                                                )
                                                .changed();
                                        });

                                        changed |= ui.checkbox(&mut self.config.emulate_psn, "PSN oturum simülasyonu (--emulate-psn)").changed();
                                        changed |= ui.checkbox(&mut self.config.network_connected, "Yerel ağ bağlantısı (--network-connected)").changed();
                                        changed |= ui.checkbox(&mut self.config.skip_notice_screen, "Başlangıç uyarılarını atla (--skip-notice-screen)").changed();
                                        if ui.checkbox(&mut self.config.show_demo_games, "Oyun yokken PS5 önizleme kartlarını göster").changed() {
                                            changed = true;
                                            self.refresh_games();
                                        }
                                        changed |= ui.checkbox(&mut self.config.skip_boot_intro, "PS5 açılış animasyonunu atla").changed();
                                    }
                                    SettingsCategory::Display => {
                                        ui.label(RichText::new("Çözünürlük").font(font_semilight(14.0)).color(Color32::WHITE));
                                        ui.horizontal(|ui| {
                                            for (w, h, lbl) in [
                                                (1280, 720, "720p"),
                                                (1920, 1080, "1080p"),
                                                (2560, 1440, "1440p"),
                                                (3840, 2160, "4K"),
                                            ] {
                                                let sel = self.config.screen_width == w && self.config.screen_height == h;
                                                if ui.selectable_label(sel, lbl).clicked() {
                                                    self.config.screen_width = w;
                                                    self.config.screen_height = h;
                                                    changed = true;
                                                }
                                            }
                                        });

                                        ui.horizontal(|ui| {
                                            ui.label("VBlank:");
                                            for hz in [60, 120] {
                                                if ui.selectable_label(self.config.vblank_frequency == hz, format!("{hz} Hz")).clicked() {
                                                    self.config.vblank_frequency = hz;
                                                    changed = true;
                                                }
                                            }
                                        });

                                        changed |= ui.checkbox(&mut self.config.fullscreen, "Çerçevesiz Tam Ekran (--fullscreen)").changed();
                                        changed |= ui.checkbox(&mut self.config.hdr, "HDR10 Çıkışı (--hdr)").changed();
                                        changed |= ui.checkbox(&mut self.config.shader_cache, "Shader Önbelleği (--shader-cache)").changed();
                                        changed |= ui.checkbox(&mut self.config.perf_overlay, "Performans Paneli (--perf-overlay)").changed();
                                        changed |= ui.checkbox(&mut self.config.native_rt, "Native Ray Tracing (--native-rt)").changed();
                                        changed |= ui.checkbox(&mut self.config.amd_cpu, "AMD CPU Yamaları (--amd-cpu)").changed();
                                    }
                                    SettingsCategory::Ps5Pro => {
                                        changed |= ui.checkbox(&mut self.config.trinity_mode, "Deneysel PS5 Pro (Trinity) Modu (--trinity)").changed();
                                        ui.add_space(6.0);
                                        ui.label(RichText::new("PSSR Yükseltici (--pssr-upscaler)").font(font_semilight(14.0)).color(Color32::WHITE));
                                        for opt in [PssrUpscaler::Linear, PssrUpscaler::DLSS, PssrUpscaler::FSR] {
                                            if ui.radio_value(&mut self.config.pssr_upscaler, opt, opt.label()).changed() {
                                                changed = true;
                                            }
                                        }
                                    }
                                    SettingsCategory::Controller => {
                                        ui.label(RichText::new("DualSense Titreşim").font(font_semilight(14.0)).color(Color32::WHITE));
                                        changed |= ui.add(egui::Slider::new(&mut self.config.controller_vibration, 0..=100).suffix("%")).changed();
                                        ui.label(RichText::new("DualSense Hoparlör Sesi").font(font_semilight(14.0)).color(Color32::WHITE));
                                        changed |= ui.add(egui::Slider::new(&mut self.config.controller_volume, 0..=100).suffix("%")).changed();
                                        ui.horizontal(|ui| {
                                            ui.label("Lightbar Rengi:");
                                            changed |= ui
                                                .add_sized(Vec2::new(110.0, 26.0), egui::TextEdit::singleline(&mut self.config.controller_color))
                                                .changed();
                                        });
                                        ui.horizontal(|ui| {
                                            for (name, hex) in [
                                                ("PS Mavi", "#0070D1"),
                                                ("Neon Mor", "#9B4DFF"),
                                                ("Kırmızı", "#E63946"),
                                                ("Altın", "#F4B942"),
                                            ] {
                                                if ui.button(name).clicked() {
                                                    self.config.controller_color = hex.to_string();
                                                    changed = true;
                                                }
                                            }
                                        });
                                    }
                                }
                            });
                        });

                        if changed {
                            self.save_config();
                        }
                    });
            });
    }

    fn draw_toasts(&mut self, ctx: &Context) {
        self.toasts.retain(|t| t.created_at.elapsed() < Duration::from_secs(4));
        if self.toasts.is_empty() {
            return;
        }

        egui::Area::new(Id::new("ps5_toasts"))
            .order(egui::Order::Tooltip)
            .anchor(Align2::RIGHT_TOP, Vec2::new(-36.0, 82.0))
            .show(ctx, |ui| {
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 10.0;
                    for toast in &self.toasts {
                        let accent = if toast.is_error {
                            Color32::from_rgb(235, 82, 82)
                        } else {
                            Color32::from_rgb(76, 175, 255)
                        };
                        egui::Frame::new()
                            .fill(Color32::from_rgba_unmultiplied(16, 22, 36, 240))
                            .stroke(Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 36)))
                            .corner_radius(CornerRadius::same(16))
                            .inner_margin(egui::Margin::symmetric(16, 12))
                            .show(ui, |ui| {
                                ui.set_min_width(290.0);
                                ui.set_max_width(370.0);
                                ui.horizontal(|ui| {
                                    let (bar_rect, _) = ui.allocate_exact_size(Vec2::new(3.5, 34.0), Sense::hover());
                                    ui.painter().rect_filled(bar_rect, CornerRadius::same(2), accent);
                                    ui.add_space(6.0);
                                    ui.vertical(|ui| {
                                        ui.label(RichText::new(&toast.title).font(font_semilight(14.0)).color(Color32::WHITE));
                                        ui.label(
                                            RichText::new(&toast.body)
                                                .font(font_semilight(12.0))
                                                .color(Color32::from_rgb(195, 208, 230)),
                                        );
                                    });
                                });
                            });
                    }
                });
            });
    }
}

impl eframe::App for Ps5ShellApp {
    fn update(&mut self, ctx: &Context, _frame: &mut eframe::Frame) {
        ctx.request_repaint_after(Duration::from_millis(16));

        self.poll_running_game();
        self.handle_navigation(ctx);

        let time = ctx.input(|i| i.time);
        let (target_p, target_s) = self.current_target_colors();
        self.bg_primary = lerp_color(self.bg_primary, target_p, 0.06);
        self.bg_secondary = lerp_color(self.bg_secondary, target_s, 0.06);

        egui::CentralPanel::default()
            .frame(egui::Frame::NONE)
            .show(ctx, |ui| {
                let rect = ui.max_rect();

                if self.in_boot_intro {
                    paint_boot_intro(ui.painter(), rect, self.boot_started_at.elapsed().as_secs_f32());
                    return;
                }

                // Track smooth wallpaper crossfade when focused game changes
                if self.active_tab == TopTab::Games && self.selected_game != self.current_bg_game {
                    self.prev_bg_game = Some(self.current_bg_game);
                    self.current_bg_game = self.selected_game;
                    self.bg_fade = 0.0;
                }
                self.bg_fade = (self.bg_fade + 0.065).min(1.0);

                // 1. Dynamic PS5 backdrop + real full-bleed 16:9 key art with smooth crossfade
                paint_ps5_background(ui.painter(), rect, time, self.bg_primary, self.bg_secondary);

                if self.active_tab == TopTab::Games {
                    let mut painted_any_image = false;

                    // Previous game's wallpaper fading out underneath
                    if self.bg_fade < 0.99 {
                        if let Some(prev_idx) = self.prev_bg_game {
                            if let Some(prev_game) = self.games.get(prev_idx) {
                                if paint_game_hero_image(ui, rect, prev_game, 255) {
                                    painted_any_image = true;
                                }
                            }
                        }
                    }

                    // Current game's wallpaper fading in on top
                    if let Some(game) = self.games.get(self.selected_game) {
                        let alpha = (self.bg_fade * 255.0).round() as u8;
                        if paint_game_hero_image(ui, rect, game, alpha) {
                            painted_any_image = true;
                        } else if !painted_any_image {
                            paint_hero_scenic_art(ui.painter(), rect, game, time);
                        }
                    }

                    // Authentic PS5 cinematic scrim over full-bleed artwork (keeps top bar & hero text ultra-crisp)
                    if painted_any_image {
                        // Top bar subtle darkening
                        let top_scrim = Rect::from_min_size(rect.min, Vec2::new(rect.width(), 150.0));
                        paint_vertical_gradient(
                            ui.painter(),
                            top_scrim,
                            Color32::from_rgba_unmultiplied(5, 8, 15, 170),
                            Color32::from_rgba_unmultiplied(5, 8, 15, 0),
                        );
                        // Bottom cinematic PS5 gradient for title + Play button
                        let bottom_scrim = Rect::from_min_max(
                            Pos2::new(rect.left(), rect.top() + rect.height() * 0.36),
                            rect.max,
                        );
                        paint_vertical_gradient(
                            ui.painter(),
                            bottom_scrim,
                            Color32::from_rgba_unmultiplied(6, 9, 16, 0),
                            Color32::from_rgba_unmultiplied(4, 7, 14, 232),
                        );
                        // Subtle left-side darkening so lower-left hero title pops like a movie poster
                        let left_glow = Pos2::new(rect.left() + rect.width() * 0.14, rect.bottom() - 120.0);
                        ui.painter().circle_filled(
                            left_glow,
                            rect.height() * 0.52,
                            Color32::from_rgba_unmultiplied(4, 7, 14, 115),
                        );
                    }
                }

                // 2. Top Bar
                self.draw_top_bar(ui, rect);

                // 3. Main Ribbon + Hero
                match self.active_tab {
                    TopTab::Games => self.draw_games_ribbon_and_hero(ui, rect, time),
                    TopTab::Media => self.draw_media_ribbon_and_hero(ui, rect, time),
                }

                // 4. Bottom Hint Bar
                self.draw_bottom_hint_bar(ui, rect);
            });

        match self.overlay {
            OverlayScreen::None => {}
            OverlayScreen::ControlCenter => self.draw_control_center(ctx),
            OverlayScreen::Settings => self.draw_settings_screen(ctx),
            OverlayScreen::GameOptions => self.draw_game_options_modal(ctx),
        }

        self.draw_toasts(ctx);
    }
}

fn paint_game_hero_image(ui: &mut egui::Ui, rect: Rect, game: &GameEntry, alpha: u8) -> bool {
    // Compute 16:9 cover-fit rect centered in the viewport so key art never stretches
    let target_aspect = 16.0_f32 / 9.0_f32;
    let view_aspect = rect.width() / rect.height().max(1.0);
    let draw_size = if view_aspect > target_aspect {
        Vec2::new(rect.width(), rect.width() / target_aspect)
    } else {
        Vec2::new(rect.height() * target_aspect, rect.height())
    };
    let draw_rect = Rect::from_center_size(rect.center(), draw_size);
    let tint = Color32::from_rgba_unmultiplied(255, 255, 255, alpha);

    if let Some(bg_path) = &game.bg_path {
        let uri = format!("file://{}", bg_path.display());
        let mut clipped_ui = ui.new_child(egui::UiBuilder::new().max_rect(rect));
        clipped_ui.set_clip_rect(rect);
        egui::Image::new(uri)
            .tint(tint)
            .fit_to_exact_size(draw_rect.size())
            .paint_at(&clipped_ui, draw_rect);
        return true;
    }

    if let Some((uri, bytes)) = game.embedded_bg {
        let mut clipped_ui = ui.new_child(egui::UiBuilder::new().max_rect(rect));
        clipped_ui.set_clip_rect(rect);
        egui::Image::from_bytes(uri, bytes)
            .tint(tint)
            .fit_to_exact_size(draw_rect.size())
            .paint_at(&clipped_ui, draw_rect);
        return true;
    }

    false
}

fn draw_ps5_game_tile(
    ui: &mut egui::Ui,
    rect: Rect,
    game: &GameEntry,
    selected: bool,
    hovered: bool,
    anim: f32,
    time: f64,
) {
    let painter = ui.painter();
    let radius: u8 = if selected { 18 } else { 14 };
    let cr = CornerRadius::same(radius);

    // Soft elevation drop shadow under focused card
    if anim > 0.05 {
        painter.rect_filled(
            rect.expand(3.0 * anim).translate(Vec2::new(0.0, 8.0 * anim)),
            CornerRadius::same(radius + 4),
            Color32::from_rgba_unmultiplied(0, 0, 0, (125.0 * anim) as u8),
        );
    }

    // True rounded-corner gradient base (zero sharp corners!)
    paint_rounded_card_base(painter, rect, radius, game.accent_secondary, game.accent_primary);

    // Real icon0.png, embedded generated cover art, or fallback vector artwork
    let mut painted_cover = false;
    if let Some(icon_path) = &game.icon_path {
        let uri = format!("file://{}", icon_path.display());
        egui::Image::new(uri)
            .corner_radius(cr)
            .fit_to_exact_size(rect.size())
            .paint_at(ui, rect);
        painted_cover = true;
    } else if let Some((uri, bytes)) = game.embedded_icon {
        egui::Image::from_bytes(uri, bytes)
            .corner_radius(cr)
            .fit_to_exact_size(rect.size())
            .paint_at(ui, rect);
        painted_cover = true;
    } else {
        paint_game_tile_artwork(painter, rect, game, selected, time);
    }

    // Crisp tiny PS5 badge on top-left corner of real cover art tiles
    if painted_cover && !matches!(game.kind, GameKind::AddGameTile | GameKind::LibraryTile) {
        let pill = Rect::from_min_size(Pos2::new(rect.left() + 8.0, rect.top() + 8.0), Vec2::new(28.0, 14.0));
        painter.rect_filled(pill, CornerRadius::same(4), Color32::from_rgba_unmultiplied(255, 255, 255, 225));
        painter.text(
            pill.center(),
            Align2::CENTER_CENTER,
            "PS5",
            font_bold(8.5),
            Color32::from_rgb(12, 16, 26),
        );
    }

    // Crisp hairline inner rim
    painter.rect_stroke(
        rect,
        cr,
        Stroke::new(
            1.0_f32,
            Color32::from_rgba_unmultiplied(255, 255, 255, if hovered { 95 } else { 36 }),
        ),
        StrokeKind::Inside,
    );

    // Authentic PS5 razor-sharp white outer focus ring with whisper-soft breathing halo
    if anim > 0.05 {
        let pulse = 0.72 + 0.28 * (time as f32 * 3.6).sin();
        painter.rect_stroke(
            rect.expand(6.0),
            CornerRadius::same(radius + 5),
            Stroke::new(
                3.5_f32,
                Color32::from_rgba_unmultiplied(150, 210, 255, (48.0 * pulse * anim) as u8),
            ),
            StrokeKind::Outside,
        );
        painter.rect_stroke(
            rect.expand(4.0),
            CornerRadius::same(radius + 3),
            Stroke::new(
                2.0_f32,
                Color32::from_rgba_unmultiplied(255, 255, 255, (245.0 * anim) as u8),
            ),
            StrokeKind::Outside,
        );
    }
}

fn draw_ps5_media_tile(
    ui: &mut egui::Ui,
    rect: Rect,
    item: &MediaEntry,
    selected: bool,
    hovered: bool,
    anim: f32,
    time: f64,
) {
    let painter = ui.painter();
    let radius: u8 = if selected { 18 } else { 14 };
    let cr = CornerRadius::same(radius);

    paint_rounded_card_base(painter, rect, radius, item.accent_secondary, item.accent_primary);

    painter.text(
        rect.center() - Vec2::new(0.0, 6.0),
        Align2::CENTER_CENTER,
        &item.icon_glyph,
        font_light(if selected { 34.0 } else { 24.0 }),
        Color32::WHITE,
    );
    painter.text(
        Pos2::new(rect.center().x, rect.bottom() - 16.0),
        Align2::CENTER_CENTER,
        &item.title,
        font_semilight(if selected { 12.0 } else { 10.0 }),
        Color32::from_rgba_unmultiplied(245, 248, 255, 225),
    );

    painter.rect_stroke(
        rect,
        cr,
        Stroke::new(
            1.0_f32,
            Color32::from_rgba_unmultiplied(255, 255, 255, if hovered { 95 } else { 36 }),
        ),
        StrokeKind::Inside,
    );

    if anim > 0.05 {
        let pulse = 0.72 + 0.28 * (time as f32 * 3.6).sin();
        painter.rect_stroke(
            rect.expand(4.0),
            CornerRadius::same(radius + 3),
            Stroke::new(
                2.0_f32,
                Color32::from_rgba_unmultiplied(255, 255, 255, (240.0 * pulse * anim) as u8),
            ),
            StrokeKind::Outside,
        );
    }
}

fn ps5_activity_card(
    ui: &mut egui::Ui,
    kicker: &str,
    value: &str,
    sub: &str,
    active: bool,
) -> bool {
    let size = Vec2::new(196.0, 96.0);
    let (rect, resp) = ui.allocate_exact_size(size, Sense::click());
    let painter = ui.painter();
    let hover = resp.hovered();

    let fill = if active {
        Color32::from_rgba_unmultiplied(0, 118, 220, 128)
    } else {
        Color32::from_rgba_unmultiplied(18, 25, 40, 224)
    };
    painter.rect_filled(rect, CornerRadius::same(16), fill);
    painter.rect_stroke(
        rect,
        CornerRadius::same(16),
        Stroke::new(
            if hover { 1.8_f32 } else { 1.0_f32 },
            Color32::from_rgba_unmultiplied(255, 255, 255, if hover { 225 } else { 42 }),
        ),
        StrokeKind::Inside,
    );

    painter.text(
        Pos2::new(rect.left() + 16.0, rect.top() + 22.0),
        Align2::LEFT_CENTER,
        kicker,
        font_bold(10.0),
        Color32::from_rgb(150, 212, 255),
    );
    painter.text(
        Pos2::new(rect.left() + 16.0, rect.top() + 48.0),
        Align2::LEFT_CENTER,
        value,
        font_light(21.0),
        Color32::WHITE,
    );
    painter.text(
        Pos2::new(rect.left() + 16.0, rect.bottom() - 18.0),
        Align2::LEFT_CENTER,
        sub,
        font_semilight(11.5),
        Color32::from_rgba_unmultiplied(205, 218, 238, 175),
    );

    resp.clicked()
}

fn dock_circle_btn(ui: &mut egui::Ui, glyph: &str, label: &str, danger: bool) -> bool {
    let mut clicked = false;
    ui.vertical(|ui| {
        ui.set_width(58.0);
        let (rect, resp) = ui.allocate_exact_size(Vec2::splat(46.0), Sense::click());
        let hover = resp.hovered();
        let c = rect.center();
        let fill = if danger {
            Color32::from_rgba_unmultiplied(220, 56, 68, if hover { 220 } else { 155 })
        } else if hover {
            Color32::from_rgba_unmultiplied(255, 255, 255, 235)
        } else {
            Color32::from_rgba_unmultiplied(255, 255, 255, 22)
        };
        ui.painter().circle_filled(c, 22.0, fill);
        ui.painter().text(
            c,
            Align2::CENTER_CENTER,
            glyph,
            font_light(19.0),
            if hover && !danger { Color32::from_rgb(12, 16, 26) } else { Color32::WHITE },
        );
        ui.add_space(4.0);
        ui.centered_and_justified(|ui| {
            ui.label(
                RichText::new(label)
                    .font(font_semilight(10.5))
                    .color(Color32::from_rgba_unmultiplied(215, 226, 244, 185)),
            );
        });
        clicked = resp.clicked();
    });
    clicked
}

fn modal_pill_btn(ui: &mut egui::Ui, label: &str) -> bool {
    let btn = egui::Button::new(RichText::new(label).font(font_semilight(14.5)).color(Color32::WHITE))
        .fill(Color32::from_rgba_unmultiplied(255, 255, 255, 20))
        .stroke(Stroke::new(1.0_f32, Color32::from_rgba_unmultiplied(255, 255, 255, 36)))
        .corner_radius(CornerRadius::same(18))
        .min_size(Vec2::new(340.0, 40.0));
    ui.add(btn).clicked()
}

fn configure_ps5_style(ctx: &Context) {
    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = Color32::from_rgb(6, 9, 18);
    visuals.window_fill = Color32::from_rgb(14, 19, 32);
    visuals.widgets.inactive.bg_fill = Color32::from_rgba_unmultiplied(255, 255, 255, 18);
    visuals.widgets.hovered.bg_fill = Color32::from_rgba_unmultiplied(255, 255, 255, 34);
    visuals.widgets.active.bg_fill = Color32::from_rgb(0, 112, 209);
    visuals.selection.bg_fill = Color32::from_rgb(0, 112, 209);
    ctx.set_visuals(visuals);
}
