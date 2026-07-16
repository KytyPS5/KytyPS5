#ifndef LAUNCHER_INCLUDE_CONFIGURATION_H_
#define LAUNCHER_INCLUDE_CONFIGURATION_H_

#include "common.h"

#include <QByteArray>
#include <QChar>
#include <QMetaEnum>
#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#define KYTY_CFG_SET(n) s->setValue(#n, QVariant::fromValue(n).toString());
#define KYTY_CFG_GET(n) n = s->value(#n).value<decltype(n)>();

template <class T>
inline QStringList EnumToList() {
	QStringList ret;
	auto        me    = QMetaEnum::fromType<T>();
	int         count = me.keyCount();
	for (int i = 0; i < count; i++) {
		auto key = QString(me.key(i));
		ret << (key.startsWith('R') && key.size() > 2 && key.at(1).isDigit()
		            ? key.remove('R').toLower()
		            : key);
	}
	return ret;
}

template <class T>
T TextToEnum(const QString& text) {
	auto me = QMetaEnum::fromType<T>();
	return static_cast<T>(me.keyToValue(
	    ((text.size() > 1 && text.at(0).isDigit()) ? 'R' + text.toUpper() : text).toUtf8().data()));
}

template <class T>
QString EnumToText(T value) {
	auto me  = QMetaEnum::fromType<T>();
	auto key = QString(me.valueToKey(static_cast<int>(value)));
	return (key.startsWith('R') && key.size() > 2 && key.at(1).isDigit() ? key.remove('R').toLower()
	                                                                     : key);
}

class Configuration: public QObject {
	Q_OBJECT

public:
	enum class Resolution {
		R1280X720,
		R1920X1080,
	};
	Q_ENUM(Resolution)

	enum class ShaderOptimizationType { None, Size, Performance };
	Q_ENUM(ShaderOptimizationType)

	enum class ShaderLogDirection { Silent, Console, File };
	Q_ENUM(ShaderLogDirection)

	enum class ProfilerDirection { None, Network };
	Q_ENUM(ProfilerDirection)

	enum class LogDirection { Silent, Console, File };
	Q_ENUM(LogDirection)

	enum class GameStatus { Unknown, InGame, Logo, DoesntBoot, MainMenu };
	Q_ENUM(GameStatus)

	Configuration() = default;

	QString    name;
	QString    title_id;    /* Serial / title id from sce_sys/param.json */
	QString    firmwareVer; /* requiredSystemSoftwareVersion from sce_sys/param.json */
	QString    basedir;     /* Game base directory */
	QString    game_path;   /* Launcher-unique game path */
	bool       custom_settings = false;
	GameStatus game_status     = GameStatus::Unknown;
	QString    game_comment;

	Resolution             screen_resolution           = Resolution::R1280X720;
	int                    vblank_frequency            = 60;
	bool                   vulkan_validation_enabled   = true;
	bool                   shader_validation_enabled   = true;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::Performance;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	QString                shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	QString                command_buffer_dump_folder  = "_Buffers";
	LogDirection           printf_direction            = LogDirection::Silent;
	QString                printf_output_file          = "_kyty.txt";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	bool                   renderdoc_enabled           = false;
	bool                   ngg_rectlist_draw_enabled   = true;

	QString elf = QStringLiteral("eboot.bin");

	void CopyEmulatorSettingsFrom(const Configuration& other) {
		screen_resolution           = other.screen_resolution;
		vblank_frequency            = other.vblank_frequency;
		vulkan_validation_enabled   = other.vulkan_validation_enabled;
		shader_validation_enabled   = other.shader_validation_enabled;
		shader_optimization_type    = other.shader_optimization_type;
		shader_log_direction        = other.shader_log_direction;
		shader_log_folder           = other.shader_log_folder;
		command_buffer_dump_enabled = other.command_buffer_dump_enabled;
		command_buffer_dump_folder  = other.command_buffer_dump_folder;
		printf_direction            = other.printf_direction;
		printf_output_file          = other.printf_output_file;
		profiler_direction          = other.profiler_direction;
		renderdoc_enabled           = other.renderdoc_enabled;
		ngg_rectlist_draw_enabled   = other.ngg_rectlist_draw_enabled;
	}

	void CopyFrom(const Configuration& other) {
		name            = other.name;
		title_id        = other.title_id;
		firmwareVer     = other.firmwareVer;
		basedir         = other.basedir;
		game_path       = other.game_path;
		custom_settings = other.custom_settings;
		game_status     = other.game_status;
		game_comment    = other.game_comment;
		CopyEmulatorSettingsFrom(other);
		elf = other.elf;
	}

	void WriteSettings(QSettings* s) const {
		KYTY_CFG_SET(name);
		KYTY_CFG_SET(basedir);
		KYTY_CFG_SET(game_path);
		KYTY_CFG_SET(custom_settings);
		KYTY_CFG_SET(screen_resolution);
		KYTY_CFG_SET(vblank_frequency);
		KYTY_CFG_SET(vulkan_validation_enabled);
		KYTY_CFG_SET(shader_validation_enabled);
		KYTY_CFG_SET(shader_optimization_type);
		KYTY_CFG_SET(shader_log_direction);
		KYTY_CFG_SET(shader_log_folder);
		KYTY_CFG_SET(command_buffer_dump_enabled);
		KYTY_CFG_SET(command_buffer_dump_folder);
		KYTY_CFG_SET(printf_direction);
		KYTY_CFG_SET(printf_output_file);
		KYTY_CFG_SET(profiler_direction);
		KYTY_CFG_SET(renderdoc_enabled);
		KYTY_CFG_SET(ngg_rectlist_draw_enabled);
		KYTY_CFG_SET(elf);
	}

	void ReadSettings(QSettings* s) {
		KYTY_CFG_GET(name);
		KYTY_CFG_GET(basedir);
		KYTY_CFG_GET(game_path);
		KYTY_CFG_GET(custom_settings);
		KYTY_CFG_GET(screen_resolution);
		vblank_frequency = s->value("vblank_frequency", vblank_frequency).toInt();
		KYTY_CFG_GET(vulkan_validation_enabled);
		KYTY_CFG_GET(shader_validation_enabled);
		KYTY_CFG_GET(shader_optimization_type);
		KYTY_CFG_GET(shader_log_direction);
		KYTY_CFG_GET(shader_log_folder);
		KYTY_CFG_GET(command_buffer_dump_enabled);
		KYTY_CFG_GET(command_buffer_dump_folder);
		KYTY_CFG_GET(printf_direction);
		KYTY_CFG_GET(printf_output_file);
		KYTY_CFG_GET(profiler_direction);
		KYTY_CFG_GET(renderdoc_enabled);
		ngg_rectlist_draw_enabled =
		    s->value("ngg_rectlist_draw_enabled", ngg_rectlist_draw_enabled).toBool();
		elf = s->value("elf", elf).toString();
	}
};

Q_DECLARE_METATYPE(Configuration*)

#endif /* LAUNCHER_INCLUDE_CONFIGURATION_H_ */
