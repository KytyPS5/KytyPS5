#ifndef LAUNCHER_INCLUDE_CONFIGURATION_H_
#define LAUNCHER_INCLUDE_CONFIGURATION_H_

#include "common/emulatorConfig.h"

#include <QByteArray>
#include <QChar>
#include <QColor>
#include <QJsonObject>
#include <QMetaEnum>
#include <QMetaType>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

template <class T>
inline QStringList EnumToList() {
	QStringList ret;
	auto        me    = QMetaEnum::fromType<T>();
	int         count = me.keyCount();
	for (int i = 0; i < count; i++) {
		ret << QString(me.key(i));
	}
	return ret;
}

template <class T>
T TextToEnum(const QString& text) {
	auto me = QMetaEnum::fromType<T>();
	return static_cast<T>(me.keyToValue(text.toUtf8().constData()));
}

template <class T>
QString EnumToText(T value) {
	return QString(QMetaEnum::fromType<T>().valueToKey(static_cast<int>(value)));
}

struct ControllerSettings {
	QString color;
	int     speaker_volume      = 50;
	int     vibration_intensity = 100;
	// 0 shares player 1 with the first gamepad; 1..MAX_PLAYERS dedicates that player to the keyboard.
	int     keyboard_player     = 0;

	void WriteSettings(QSettings* s) const {
		s->setValue("controller_color", color);
		s->setValue("controller_speaker_volume", speaker_volume);
		s->setValue("controller_vibration_intensity", vibration_intensity);
		s->setValue("controller_keyboard_player", keyboard_player);
	}

	void ReadSettings(QSettings* s) {
		const QColor saved_color(s->value("controller_color").toString());
		color = saved_color.isValid() ? saved_color.name(QColor::HexRgb) : QString {};
		const auto read_percent = [s](const char* key, int fallback) {
			bool      ok    = false;
			const int value = s->value(key, fallback).toInt(&ok);
			return ok ? qBound(0, value, 100) : fallback;
		};
		speaker_volume      = read_percent("controller_speaker_volume", 50);
		vibration_intensity = read_percent("controller_vibration_intensity", 100);
		keyboard_player = qBound(0, s->value("controller_keyboard_player", 0).toInt(),
		                         Config::MAX_PLAYERS);
	}
};

class Configuration: public QObject {
	Q_OBJECT

public:
	static constexpr int DEFAULT_CONSOLE_LANGUAGE = 1;
	static constexpr int MAX_CONSOLE_LANGUAGE     = 29;

	static constexpr int MIN_SCREEN_SIZE = 320;
	static constexpr int MAX_SCREEN_SIZE = 16384;

	static QString NormalizeResolution(const QString& text);

	enum class ShaderOptimizationType { None, Size, Performance };
	Q_ENUM(ShaderOptimizationType)

	enum class PresentMode { Fifo, Mailbox, Immediate };
	Q_ENUM(PresentMode)

	enum class LogDirection { Silent, Console, File };
	Q_ENUM(LogDirection)

	enum class GameStatus { Unknown, InGame, Logo, DoesntBoot, MainMenu };
	Q_ENUM(GameStatus)

	Configuration() = default;

	QString     name;
	QString     title_id;    /* Serial / title id from sce_sys/param.json */
	QString     gameVersion; /* appVersion / contentVersion from sce_sys/param.json */
	QString     firmwareVer; /* requiredSystemSoftwareVersion from sce_sys/param.json */
	QString     basedir;     /* Game base directory */
	QString     game_path;   /* Launcher-unique game path */
	QString     addons_dir;  /* Global setting; not saved per game. */
	QStringList addcont;     /* User add-ons ("LABEL" or "LABEL=DIR"), saved by title ID. */
	bool        custom_settings = false;
	GameStatus  game_status     = GameStatus::Unknown;
	QString     game_comment;

	// Controller preferences always come from the global configuration.
	ControllerSettings controller;

	QString                screen_resolution           = QStringLiteral("1280x720");
	QString                user_name                   = "Kyty";
	int                    user_id                     = Config::DEFAULT_USER_ID;
	QString                audio_input_device;
	PresentMode            present_mode                = PresentMode::Mailbox;
	int                    gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	bool                   hide_cursor_enabled         = false;
	bool                   stretch_enabled             = false;
	bool                   readback_linear_images      = false;
	bool                   readback_tiled_images       = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   occlusion_queries_enabled   = false;
	bool                   shader_cache_enabled        = true;
	bool                   trophy_enabled              = true;
	int                    trophy_duration_seconds     = 6;
	bool                   perf_overlay_enabled        = false;
	bool                   skip_notice_screen          = false;
	bool                   network_connected           = false;
	int                    vblank_frequency            = 60;
	int                    console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::Performance;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	QString                shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	QString                command_buffer_dump_folder  = "_Buffers";
	LogDirection           printf_direction            = LogDirection::Silent;
	QString                printf_output_file          = "_kyty.txt";
	bool                   profiler_enabled            = false;
	bool                   renderdoc_enabled           = false;
	bool                   amd_cpu_enabled             = false;
#if defined(_WIN32)
	bool red_zone_protection_enabled = false;
#endif
	QStringList host_input_mapping;

	QString elf = QStringLiteral("eboot.bin");

	void CopyEmulatorSettingsFrom(const Configuration& other) {
		screen_resolution           = other.screen_resolution;
		user_name                   = other.user_name;
		user_id                     = other.user_id;
		audio_input_device          = other.audio_input_device;
		present_mode                = other.present_mode;
		gpu_index                   = other.gpu_index;
		fullscreen_enabled          = other.fullscreen_enabled;
		hdr_enabled                 = other.hdr_enabled;
		hide_cursor_enabled         = other.hide_cursor_enabled;
		stretch_enabled             = other.stretch_enabled;
		readback_linear_images      = other.readback_linear_images;
		readback_tiled_images       = other.readback_tiled_images;
		sync_raw_image_buffers      = other.sync_raw_image_buffers;
		tessellation_enabled        = other.tessellation_enabled;
		occlusion_queries_enabled   = other.occlusion_queries_enabled;
		shader_cache_enabled        = other.shader_cache_enabled;
		trophy_enabled              = other.trophy_enabled;
		trophy_duration_seconds     = other.trophy_duration_seconds;
		perf_overlay_enabled        = other.perf_overlay_enabled;
		skip_notice_screen          = other.skip_notice_screen;
		network_connected           = other.network_connected;
		vblank_frequency            = other.vblank_frequency;
		console_language            = other.console_language;
		vulkan_validation_enabled   = other.vulkan_validation_enabled;
		shader_validation_enabled   = other.shader_validation_enabled;
		shader_optimization_type    = other.shader_optimization_type;
		shader_log_direction        = other.shader_log_direction;
		shader_log_folder           = other.shader_log_folder;
		command_buffer_dump_enabled = other.command_buffer_dump_enabled;
		command_buffer_dump_folder  = other.command_buffer_dump_folder;
		printf_direction            = other.printf_direction;
		printf_output_file          = other.printf_output_file;
		profiler_enabled            = other.profiler_enabled;
		renderdoc_enabled           = other.renderdoc_enabled;
		amd_cpu_enabled             = other.amd_cpu_enabled;
#if defined(_WIN32)
		red_zone_protection_enabled = other.red_zone_protection_enabled;
#endif
		host_input_mapping = other.host_input_mapping;
	}

	void CopyGameInfoFrom(const Configuration& other) {
		name            = other.name;
		title_id        = other.title_id;
		gameVersion     = other.gameVersion;
		firmwareVer     = other.firmwareVer;
		basedir         = other.basedir;
		game_path       = other.game_path;
		custom_settings = other.custom_settings;
		game_status     = other.game_status;
		game_comment    = other.game_comment;
	}

	void WriteSettings(QSettings* s) const;
	void ReadSettings(QSettings* s);

	[[nodiscard]] QVariantMap GameSettings() const;
	// Call on a temporary configuration: validation can fail after loading values.
	bool SetGameSettings(const QJsonObject& settings, QString& error);

private:
	template <class Settings>
	void ReadGameSettingsValues(const Settings& s);
};

Q_DECLARE_METATYPE(Configuration*)

#endif /* LAUNCHER_INCLUDE_CONFIGURATION_H_ */
