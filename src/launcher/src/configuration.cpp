#include "configuration.h"

#include <QRegularExpression>

QString Configuration::NormalizeResolution(const QString& text) {
	// Older settings stored the enum key, e.g. "R1920X1080".
	static const QRegularExpression re(QStringLiteral("^\\s*R?(\\d+)\\s*[xX]\\s*(\\d+)\\s*$"));
	const auto match = re.match(text);
	if (!match.hasMatch()) {
		return {};
	}
	const int width  = match.captured(1).toInt();
	const int height = match.captured(2).toInt();
	if (width < MIN_SCREEN_SIZE || height < MIN_SCREEN_SIZE || width > MAX_SCREEN_SIZE ||
	    height > MAX_SCREEN_SIZE) {
		return {};
	}
	return QStringLiteral("%1x%2").arg(width).arg(height);
}

QVariantMap Configuration::GameSettings() const {
	QVariantMap values;
#define KYTY_CFG_SET(n) values.insert(QStringLiteral(#n), QVariant::fromValue(n).toString())
	KYTY_CFG_SET(screen_resolution);
	KYTY_CFG_SET(user_name);
	KYTY_CFG_SET(user_id);
	KYTY_CFG_SET(audio_input_device);
	KYTY_CFG_SET(present_mode);
	KYTY_CFG_SET(gpu_index);
	KYTY_CFG_SET(fullscreen_enabled);
	KYTY_CFG_SET(hdr_enabled);
	KYTY_CFG_SET(hide_cursor_enabled);
	KYTY_CFG_SET(stretch_enabled);
	KYTY_CFG_SET(readback_linear_images);
	KYTY_CFG_SET(readback_tiled_images);
	KYTY_CFG_SET(sync_raw_image_buffers);
	KYTY_CFG_SET(tessellation_enabled);
	KYTY_CFG_SET(occlusion_queries_enabled);
	KYTY_CFG_SET(shader_cache_enabled);
	KYTY_CFG_SET(trophy_enabled);
	KYTY_CFG_SET(trophy_duration_seconds);
	KYTY_CFG_SET(perf_overlay_enabled);
	KYTY_CFG_SET(skip_notice_screen);
	KYTY_CFG_SET(network_connected);
	KYTY_CFG_SET(psn_emulation_enabled);
	KYTY_CFG_SET(vblank_frequency);
	KYTY_CFG_SET(console_language);
	KYTY_CFG_SET(vulkan_validation_enabled);
	KYTY_CFG_SET(shader_validation_enabled);
	KYTY_CFG_SET(shader_optimization_type);
	KYTY_CFG_SET(shader_log_direction);
	KYTY_CFG_SET(shader_log_folder);
	KYTY_CFG_SET(command_buffer_dump_enabled);
	KYTY_CFG_SET(command_buffer_dump_folder);
	KYTY_CFG_SET(printf_direction);
	KYTY_CFG_SET(printf_output_file);
	KYTY_CFG_SET(profiler_enabled);
	KYTY_CFG_SET(renderdoc_enabled);
	KYTY_CFG_SET(pro_enabled);
	KYTY_CFG_SET(amd_cpu_enabled);
#if defined(_WIN32)
	KYTY_CFG_SET(red_zone_protection_enabled);
#endif
	KYTY_CFG_SET(elf);
#undef KYTY_CFG_SET
	return values;
}

template <class Settings>
void Configuration::ReadGameSettingsValues(const Settings& s) {
#define KYTY_CFG_GET(n) n = s.value(QStringLiteral(#n)).template value<decltype(n)>()
	const auto resolution = NormalizeResolution(s.value("screen_resolution").toString());
	screen_resolution     = resolution.isEmpty() ? QStringLiteral("1280x720") : resolution;
	user_name          = s.value("user_name", user_name).toString();
	bool user_id_ok    = false;
	auto saved_user_id = s.value("user_id", user_id).toInt(&user_id_ok);
	user_id            = user_id_ok && Config::IsConfiguredUserIdValid(saved_user_id)
	                         ? saved_user_id
	                         : Config::DEFAULT_USER_ID;
	audio_input_device = s.value("audio_input_device", audio_input_device).toString();
	KYTY_CFG_GET(present_mode);
	gpu_index = s.value("gpu_index", -1).toInt();
	if (EnumToText(present_mode).isEmpty()) {
		present_mode = PresentMode::Mailbox;
	}
	KYTY_CFG_GET(fullscreen_enabled);
	KYTY_CFG_GET(hdr_enabled);
	KYTY_CFG_GET(hide_cursor_enabled);
	KYTY_CFG_GET(stretch_enabled);
	KYTY_CFG_GET(readback_linear_images);
	KYTY_CFG_GET(readback_tiled_images);
	KYTY_CFG_GET(sync_raw_image_buffers);
	KYTY_CFG_GET(tessellation_enabled);
	KYTY_CFG_GET(occlusion_queries_enabled);
	shader_cache_enabled = s.value("shader_cache_enabled", shader_cache_enabled).toBool();
	trophy_enabled       = s.value("trophy_enabled", trophy_enabled).toBool();
	KYTY_CFG_GET(trophy_duration_seconds);
	KYTY_CFG_GET(perf_overlay_enabled);
	KYTY_CFG_GET(skip_notice_screen);
	KYTY_CFG_GET(network_connected);
	KYTY_CFG_GET(psn_emulation_enabled);
	vblank_frequency = s.value("vblank_frequency", vblank_frequency).toInt();
	console_language = s.value("console_language", console_language).toInt();
	if (console_language < 0 || console_language > MAX_CONSOLE_LANGUAGE) {
		console_language = DEFAULT_CONSOLE_LANGUAGE;
	}
	KYTY_CFG_GET(vulkan_validation_enabled);
	KYTY_CFG_GET(shader_validation_enabled);
	KYTY_CFG_GET(shader_optimization_type);
	KYTY_CFG_GET(shader_log_direction);
	KYTY_CFG_GET(shader_log_folder);
	KYTY_CFG_GET(command_buffer_dump_enabled);
	KYTY_CFG_GET(command_buffer_dump_folder);
	KYTY_CFG_GET(printf_direction);
	KYTY_CFG_GET(printf_output_file);
	KYTY_CFG_GET(profiler_enabled);
	KYTY_CFG_GET(renderdoc_enabled);
	pro_enabled     = s.value("pro_enabled", false).toBool();
	amd_cpu_enabled = s.value("amd_cpu_enabled", false).toBool();
#if defined(_WIN32)
	red_zone_protection_enabled =
	    s.value("red_zone_protection_enabled", red_zone_protection_enabled).toBool();
#endif
	elf = s.value("elf", elf).toString();
#undef KYTY_CFG_GET
}

void Configuration::WriteSettings(QSettings* s) const {
	const auto values = GameSettings();
	for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
		s->setValue(it.key(), it.value());
	}
	s->setValue("name", name);
	s->setValue("basedir", basedir);
	s->setValue("game_path", game_path);
	s->setValue("custom_settings", QVariant::fromValue(custom_settings).toString());
	s->setValue("host_input_mapping", host_input_mapping);
}

void Configuration::ReadSettings(QSettings* s) {
	ReadGameSettingsValues(*s);
	name               = s->value("name").toString();
	basedir            = s->value("basedir").toString();
	game_path          = s->value("game_path").toString();
	custom_settings    = s->value("custom_settings").toBool();
	host_input_mapping = s->value("host_input_mapping", host_input_mapping).toStringList();
}

bool Configuration::SetGameSettings(const QJsonObject& settings, QString& error) {
	const auto fail = [&error](const QString& message) {
		error = message;
		return false;
	};
	auto values = GameSettings();
	for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
		if (!values.contains(it.key())) {
			return fail(tr("Unknown game setting: %1").arg(it.key()));
		}
		if (!it.value().isString()) {
			return fail(tr("Game setting %1 must be a string.").arg(it.key()));
		}
		const auto value = it.value().toString();
		values.insert(it.key(), it.key() == "screen_resolution" ? NormalizeResolution(value)
		                                                       : value);
	}

	ReadGameSettingsValues(values);
	const auto canonical = GameSettings();
	for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
		if (canonical.value(it.key()) != values.value(it.key())) {
			return fail(tr("Invalid value for game setting: %1").arg(it.key()));
		}
	}
	if (user_name.trimmed().isEmpty() || user_name.toUtf8().size() > Config::MAX_USER_NAME_LENGTH) {
		return fail(tr("User name must contain 1-16 UTF-8 bytes."));
	}
	if (gpu_index < -1) {
		return fail(tr("GPU index must be -1 or greater."));
	}
	if (vblank_frequency < 30 || vblank_frequency > 360) {
		return fail(tr("Vblank frequency must be between 30 and 360."));
	}
	if (trophy_duration_seconds < 5 || trophy_duration_seconds > 10) {
		return fail(tr("Trophy notification duration must be between 5 and 10 seconds."));
	}
	if ((shader_log_direction == LogDirection::File && shader_log_folder.isEmpty()) ||
	    (printf_direction == LogDirection::File && printf_output_file.isEmpty()) ||
	    (command_buffer_dump_enabled && command_buffer_dump_folder.isEmpty())) {
		return fail(tr("Enabled logging and command buffer dumps require an output path."));
	}

	return true;
}
