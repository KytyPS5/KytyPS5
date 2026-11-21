#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod artwork;
mod background;
mod config;
mod gamepad;
mod games;
mod launcher;
mod ui;

use eframe::egui;
use std::sync::Arc;
use ui::Ps5ShellApp;

fn install_system_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();

    let candidates = [
        ("segoe_ui", r"C:\Windows\Fonts\segoeui.ttf"),
        ("segoe_light", r"C:\Windows\Fonts\segoeuil.ttf"),
        ("segoe_semilight", r"C:\Windows\Fonts\segoeuisl.ttf"),
        ("segoe_bold", r"C:\Windows\Fonts\segoeuib.ttf"),
        ("segoe_sym", r"C:\Windows\Fonts\seguisym.ttf"),
    ];

    for (name, path) in candidates {
        if let Ok(bytes) = std::fs::read(path) {
            fonts
                .font_data
                .insert(name.to_owned(), Arc::new(egui::FontData::from_owned(bytes)));
        }
    }

    if fonts.font_data.contains_key("segoe_ui") {
        if let Some(family) = fonts.families.get_mut(&egui::FontFamily::Proportional) {
            family.insert(0, "segoe_ui".to_owned());
            if fonts.font_data.contains_key("segoe_sym") {
                family.push("segoe_sym".to_owned());
            }
        }
    }

    let make_family = |primary: &str| {
        let mut v = Vec::new();
        if fonts.font_data.contains_key(primary) {
            v.push(primary.to_owned());
        }
        if fonts.font_data.contains_key("segoe_ui") {
            v.push("segoe_ui".to_owned());
        }
        if fonts.font_data.contains_key("segoe_sym") {
            v.push("segoe_sym".to_owned());
        }
        v.push("Ubuntu-Light".to_owned());
        v
    };

    fonts
        .families
        .insert(egui::FontFamily::Name("light".into()), make_family("segoe_light"));
    fonts.families.insert(
        egui::FontFamily::Name("semilight".into()),
        make_family("segoe_semilight"),
    );
    fonts
        .families
        .insert(egui::FontFamily::Name("bold".into()), make_family("segoe_bold"));

    ctx.set_fonts(fonts);
}

fn main() -> eframe::Result<()> {
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("PS5 ShellUI • KytyPS5")
            .with_inner_size([1366.0, 768.0])
            .with_min_inner_size([1024.0, 600.0]),
        ..Default::default()
    };

    eframe::run_native(
        "PS5 ShellUI • KytyPS5",
        native_options,
        Box::new(|cc| {
            install_system_fonts(&cc.egui_ctx);
            Ok(Box::new(Ps5ShellApp::new(cc)))
        }),
    )
}
