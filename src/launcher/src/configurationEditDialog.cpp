#include "configurationEditDialog.h"

#include "common/emulatorConfig.h"
#include "configuration.h"
#include "mandatoryLineEdit.h"
#include <SDL3/SDL.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtAlgorithms>

#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#include <QVulkanWindow>
#endif

#include "ui_configuration_edit_dialog.h"

constexpr char SETTINGS_CFG_DIALOG[]               = "ConfigurationEditDialog";
constexpr char SETTINGS_CFG_LAST_GEOMETRY[]        = "geometry";
constexpr int  GLOBAL_SETTINGS_GAME_DIRS_MIN_WIDTH = 400;

static void UpdateControllerColorButton(QPushButton* button, const QString& hex) {
	const QColor color(hex);
	if (!color.isValid()) {
		button->setProperty("controllerColor", QString {});
		button->setText(QObject::tr("Custom"));
		button->setStyleSheet({});
		return;
	}
	const auto normalized = color.name(QColor::HexRgb);
	button->setProperty("controllerColor", normalized);
	button->setText(normalized.toUpper());
	button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
	                          .arg(normalized, color.lightness() < 128 ? QStringLiteral("white")
	                                                                   : QStringLiteral("black")));
}

const QStringList CONSOLE_LANGUAGE_NAMES = {
    "Japanese",
    "English (United States)",
    "French (France)",
    "Spanish (Spain)",
    "German",
    "Italian",
    "Dutch",
    "Portuguese (Portugal)",
    "Russian",
    "Korean",
    "Chinese (Traditional)",
    "Chinese (Simplified)",
    "Finnish",
    "Swedish",
    "Danish",
    "Norwegian",
    "Polish",
    "Portuguese (Brazil)",
    "English (United Kingdom)",
    "Turkish",
    "Spanish (Latin America)",
    "Arabic",
    "French (Canada)",
    "Czech",
    "Hungarian",
    "Greek",
    "Romanian",
    "Thai",
    "Vietnamese",
    "Indonesian",
};

static QString NormalizeGameDirectory(const QString& dir) {
	const auto trimmed = dir.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}

	return QDir::cleanPath(QDir(trimmed).absolutePath());
}

static QString GameDirectoryKey(const QString& dir) {
	const auto normalized = NormalizeGameDirectory(dir);
	if (normalized.isEmpty()) {
		return {};
	}

	auto canonical = QFileInfo(normalized).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = normalized;
	}
	canonical = QDir::cleanPath(canonical);

#ifdef __linux__
	return canonical;
#else
	return canonical.toCaseFolded();
#endif
}

ConfigurationEditDialog::ConfigurationEditDialog(Configuration& info, QWidget* parent)
    : QDialog(parent, Qt::WindowCloseButtonHint), m_ui(new Ui::ConfigurationEditDialog),
      m_info(info) {
	m_ui->setupUi(this);
	setMinimumWidth(width());
	InitGameDirectories();
	m_ui->controller_group->setVisible(false);
	m_ui->settings_button->setVisible(false);

	connect(m_ui->ok_button, &QPushButton::clicked, this, &ConfigurationEditDialog::save);
	connect(m_ui->cancel_button, &QPushButton::clicked, this, &QDialog::reject);
	connect(m_ui->clear_button, &QPushButton::clicked, this, &ConfigurationEditDialog::clear);
	connect(m_ui->button_controller_color, &QPushButton::clicked, this, [this]() {
		const auto current = m_ui->button_controller_color->property("controllerColor").toString();
		const auto initial =
		    current.isEmpty() ? QColor(QStringLiteral("#0070d1")) : QColor(current);
		const auto color = QColorDialog::getColor(initial, this, tr("DualSense lightbar color"));
		if (color.isValid()) {
			const auto hex = color.name(QColor::HexRgb);
			UpdateControllerColorButton(m_ui->button_controller_color, hex);
			emit PreviewControllerColor(hex);
		}
	});
	connect(m_ui->button_controller_color_reset, &QPushButton::clicked, this, [this]() {
		UpdateControllerColorButton(m_ui->button_controller_color, {});
		emit PreviewControllerColor({});
	});
	connect(m_ui->slider_controller_vibration, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_controller_vibration_value->setText(tr("%1%").arg(value));
	});
	connect(m_ui->slider_controller_volume, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_controller_volume_value->setText(tr("%1%").arg(value));
	});
	connect(m_ui->comboBox_shader_log_direction, &QComboBox::currentTextChanged, this,
	        [this](const QString& text) {
		        auto log = TextToEnum<Configuration::LogDirection>(text);
		        m_ui->lineEdit_shader_log_folder->setEnabled(
		            log == Configuration::LogDirection::File);
	        });
	connect(m_ui->checkBox_cmd_dump, &QCheckBox::toggled, this,
	        [this](bool flag) { m_ui->lineEdit_cmd_dump_folder->setEnabled(flag); });
	connect(m_ui->comboBox_printf_direction, &QComboBox::currentTextChanged, this,
	        [this](const QString& text) {
		        auto log = TextToEnum<Configuration::LogDirection>(text);
		        m_ui->lineEdit_printf_file->setEnabled(log == Configuration::LogDirection::File);
	        });

	setSizeGripEnabled(true);

	restoreGeometry(g_last_geometry);

	Init(info);
}

QByteArray ConfigurationEditDialog::g_last_geometry;

ConfigurationEditDialog::~ConfigurationEditDialog() {
	delete m_ui;
}

void ConfigurationEditDialog::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_CFG_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_CFG_LAST_GEOMETRY, g_last_geometry);
	}

	s.endGroup();
}

void ConfigurationEditDialog::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_CFG_DIALOG);

	g_last_geometry = s.value(SETTINGS_CFG_LAST_GEOMETRY, g_last_geometry).toByteArray();

	s.endGroup();
}

template <class T>
static void ListInit(QComboBox* combo, T value) {
	combo->clear();
	combo->addItems(EnumToList<T>());
	combo->setCurrentText(EnumToText(value));
}

void ConfigurationEditDialog::Init(const Configuration& info) {
	m_ui->lineEdit_user_name->setMaxLength(static_cast<int>(Config::MAX_USER_NAME_LENGTH));
	m_ui->lineEdit_user_name->setText(info.user_name);
	m_ui->spinBox_user_id->setValue(info.user_id);
	UpdateControllerColorButton(m_ui->button_controller_color, info.controller.color);
	m_ui->slider_controller_vibration->setValue(info.controller.vibration_intensity);
	m_ui->slider_controller_volume->setValue(info.controller.speaker_volume);
	auto* microphone = m_ui->comboBox_audio_input_device;
	microphone->clear();
	microphone->addItem(tr("None"), QString {});
	microphone->setToolTip(tr("Microphone used by games. None supplies silence."));
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		int                device_count = 0;
		SDL_AudioDeviceID* devices      = SDL_GetAudioRecordingDevices(&device_count);
		for (int i = 0; i < device_count; i++) {
			if (const auto* device = SDL_GetAudioDeviceName(devices[i]); device != nullptr) {
				const auto name = QString::fromUtf8(device);
				if (microphone->findData(name) < 0) {
					microphone->addItem(name, name);
				}
			}
		}
		SDL_free(devices);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	} else {
		microphone->setToolTip(tr("Microphones could not be listed: %1")
		                           .arg(QString::fromUtf8(SDL_GetError())));
	}
	if (microphone->findData(info.audio_input_device) < 0) {
		microphone->addItem(tr("%1 (unavailable)").arg(info.audio_input_device),
		                    info.audio_input_device);
	}
	microphone->setCurrentIndex(microphone->findData(info.audio_input_device));
	ListInit(m_ui->comboBox_screen_resolution, info.screen_resolution);
	ListInit(m_ui->comboBox_present_mode, info.present_mode);
	m_ui->comboBox_gpu->clear();
	m_ui->comboBox_gpu->addItem(tr("Auto"));
	// Keep Auto when Qt is built without Vulkan support.
#if QT_CONFIG(vulkan)
#if defined(__APPLE__)
	if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB")) {
		const auto base    = QCoreApplication::applicationDirPath();
		auto       library = base + "/libMoltenVK.dylib";
		if (!QFileInfo::exists(library)) {
			library = base + "/../Frameworks/libMoltenVK.dylib";
		}
		if (QFileInfo::exists(library)) {
			qputenv("QT_VULKAN_LIB", library.toUtf8());
		}
	}
#endif
	QVulkanInstance instance;
	instance.setApiVersion(QVersionNumber(1, 3, 0));
#if !defined(__APPLE__) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	instance.setFlags(QVulkanInstance::NoPortabilityDrivers);
#endif
	if (instance.create()) {
		QVulkanWindow window;
		window.setVulkanInstance(&instance);
		for (const auto& device: window.availablePhysicalDevices()) {
			m_ui->comboBox_gpu->addItem(QString::fromUtf8(device.deviceName));
		}
	}
#endif
	m_ui->comboBox_gpu->setCurrentIndex(
	    info.gpu_index >= 0 && info.gpu_index < m_ui->comboBox_gpu->count() - 1 ? info.gpu_index + 1
	                                                                            : 0);
	m_ui->checkBox_fullscreen->setChecked(info.fullscreen_enabled);
	m_ui->checkBox_hide_cursor->setChecked(info.hide_cursor_enabled);
	m_ui->checkBox_readback->setChecked(info.readback_linear_images);
	m_ui->checkBox_sync_raw_image_buffers->setChecked(info.sync_raw_image_buffers);
	m_ui->checkBox_tessellation->setChecked(info.tessellation_enabled);
	m_ui->checkBox_skip_notice_screen->setChecked(info.skip_notice_screen);
	m_ui->checkBox_trophy_notifications->setChecked(info.trophy_enabled);
	m_ui->spinBox_vblank_frequency->setValue(info.vblank_frequency);
	m_ui->comboBox_console_language->clear();
	m_ui->comboBox_console_language->addItems(CONSOLE_LANGUAGE_NAMES);
	m_ui->comboBox_console_language->setCurrentIndex(
	    info.console_language >= 0 && info.console_language < CONSOLE_LANGUAGE_NAMES.size()
	        ? info.console_language
	        : Configuration::DEFAULT_CONSOLE_LANGUAGE);
	m_ui->checkBox_shader_validation->setChecked(info.shader_validation_enabled);
	m_ui->checkBox_vulkan_validation->setChecked(info.vulkan_validation_enabled);
	m_ui->checkBox_renderdoc_capture->setChecked(info.renderdoc_enabled);
	m_ui->checkBox_amd_cpu->setChecked(info.amd_cpu_enabled);
#if defined(__APPLE__)
	m_ui->checkBox_amd_cpu->setVisible(false);
#endif
#if defined(_WIN32)
	m_ui->checkBox_red_zone_protection->setChecked(info.red_zone_protection_enabled);
#else
	m_ui->checkBox_red_zone_protection->setVisible(false);
#endif
	ListInit(m_ui->comboBox_shader_optimization_type, info.shader_optimization_type);
	ListInit(m_ui->comboBox_shader_log_direction, info.shader_log_direction);
	m_ui->lineEdit_shader_log_folder->setText(info.shader_log_folder);
	m_ui->lineEdit_shader_log_folder->setEnabled(info.shader_log_direction ==
	                                             Configuration::LogDirection::File);
	m_ui->checkBox_cmd_dump->setChecked(info.command_buffer_dump_enabled);
	m_ui->lineEdit_cmd_dump_folder->setText(info.command_buffer_dump_folder);
	m_ui->lineEdit_cmd_dump_folder->setEnabled(info.command_buffer_dump_enabled);
	ListInit(m_ui->comboBox_printf_direction, info.printf_direction);
	m_ui->lineEdit_printf_file->setText(info.printf_output_file);
	m_ui->lineEdit_printf_file->setEnabled(info.printf_direction ==
	                                       Configuration::LogDirection::File);
	m_ui->checkBox_profiler->setChecked(info.profiler_enabled);
}

void ConfigurationEditDialog::InitGameDirectories() {
	m_game_dirs_group = new QGroupBox(tr("Game folders"), this);

	auto* group_layout = new QVBoxLayout(m_game_dirs_group);
	group_layout->setContentsMargins(8, 8, 8, 8);
	group_layout->setSpacing(6);

	m_game_dirs_list = new QListWidget(m_game_dirs_group);
	m_game_dirs_list->setMinimumHeight(120);
	m_game_dirs_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	group_layout->addWidget(m_game_dirs_list);

	auto* button_layout = new QHBoxLayout;
	button_layout->setContentsMargins(0, 0, 0, 0);
	button_layout->setSpacing(4);

	auto* add_button = new QToolButton(m_game_dirs_group);
	add_button->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
	add_button->setToolTip(tr("Add game folder"));
	add_button->setAutoRaise(true);
	button_layout->addWidget(add_button);

	m_remove_game_dir_button = new QToolButton(m_game_dirs_group);
	m_remove_game_dir_button->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
	m_remove_game_dir_button->setToolTip(tr("Remove selected game folders"));
	m_remove_game_dir_button->setAutoRaise(true);
	button_layout->addWidget(m_remove_game_dir_button);

	button_layout->addStretch(1);
	group_layout->addLayout(button_layout);

	m_ui->gridLayout->addWidget(m_game_dirs_group, 1, 0, 1, 1);
	m_game_dirs_group->setVisible(false);

	connect(add_button, &QToolButton::clicked, this, &ConfigurationEditDialog::add_game_directory);
	connect(m_remove_game_dir_button, &QToolButton::clicked, this,
	        &ConfigurationEditDialog::remove_selected_game_directories);
	connect(m_game_dirs_list, &QListWidget::itemSelectionChanged, this,
	        &ConfigurationEditDialog::update_game_directory_buttons);

	update_game_directory_buttons();
}

void ConfigurationEditDialog::SetGlobalSettings(const QStringList& dirs) {
	m_global_settings = true;
	auto* menu        = new QMenu(m_ui->settings_button);
	menu->addAction(tr("Import..."), this, &ConfigurationEditDialog::ImportGameSettings);
	menu->addAction(tr("Export..."), this, &ConfigurationEditDialog::ExportGameSettings);
	m_ui->settings_button->setMenu(menu);
	m_ui->settings_button->setVisible(true);
	m_game_dirs_list->clear();
	m_game_dirs_group->setMinimumWidth(GLOBAL_SETTINGS_GAME_DIRS_MIN_WIDTH);

	for (const auto& dir: dirs) {
		AddGameDirectoryItem(dir);
	}

	m_game_dirs_group->setVisible(true);
	m_ui->controller_group->setVisible(true);
	update_game_directory_buttons();
	layout()->activate();
	resize(size().expandedTo(minimumSizeHint()));
}

QStringList ConfigurationEditDialog::GetGameDirectories() const {
	QStringList dirs;
	if (!m_global_settings) {
		return dirs;
	}

	for (int index = 0; index < m_game_dirs_list->count(); index++) {
		auto* item = m_game_dirs_list->item(index);
		dirs.append(item->text());
	}

	return dirs;
}

void ConfigurationEditDialog::AddGameDirectoryItem(const QString& dir) {
	const auto normalized = NormalizeGameDirectory(dir);
	const auto key        = GameDirectoryKey(normalized);
	if (normalized.isEmpty() || key.isEmpty()) {
		return;
	}

	for (int index = 0; index < m_game_dirs_list->count(); index++) {
		auto* item = m_game_dirs_list->item(index);
		if (GameDirectoryKey(item->text()) == key) {
			return;
		}
	}

	auto* item = new QListWidgetItem(normalized, m_game_dirs_list);
	item->setToolTip(normalized);
}

void ConfigurationEditDialog::moveEvent(QMoveEvent* event) {
	QDialog::moveEvent(event);
	g_last_geometry = saveGeometry();
}

void ConfigurationEditDialog::resizeEvent(QResizeEvent* event) {
	QDialog::resizeEvent(event);
	g_last_geometry = saveGeometry();
}

static void UpdateInfo(Configuration& info, Ui::ConfigurationEditDialog& ui, bool global_settings) {
	info.user_name = ui.lineEdit_user_name->text().trimmed();
	info.user_id   = ui.spinBox_user_id->value();
	info.audio_input_device = ui.comboBox_audio_input_device->currentData().toString();
	if (global_settings) {
		info.controller.color = ui.button_controller_color->property("controllerColor").toString();
		info.controller.vibration_intensity = ui.slider_controller_vibration->value();
		info.controller.speaker_volume      = ui.slider_controller_volume->value();
	}
	info.screen_resolution =
	    TextToEnum<Configuration::Resolution>(ui.comboBox_screen_resolution->currentText());
	info.present_mode =
	    TextToEnum<Configuration::PresentMode>(ui.comboBox_present_mode->currentText());
	info.gpu_index                 = ui.comboBox_gpu->currentIndex() - 1;
	info.fullscreen_enabled        = ui.checkBox_fullscreen->isChecked();
	info.hide_cursor_enabled       = ui.checkBox_hide_cursor->isChecked();
	info.readback_linear_images    = ui.checkBox_readback->isChecked();
	info.sync_raw_image_buffers    = ui.checkBox_sync_raw_image_buffers->isChecked();
	info.tessellation_enabled      = ui.checkBox_tessellation->isChecked();
	info.skip_notice_screen        = ui.checkBox_skip_notice_screen->isChecked();
	info.trophy_enabled            = ui.checkBox_trophy_notifications->isChecked();
	info.vblank_frequency          = ui.spinBox_vblank_frequency->value();
	info.console_language          = ui.comboBox_console_language->currentIndex();
	info.vulkan_validation_enabled = ui.checkBox_vulkan_validation->isChecked();
	info.shader_validation_enabled = ui.checkBox_shader_validation->isChecked();
	info.renderdoc_enabled         = ui.checkBox_renderdoc_capture->isChecked();
	info.amd_cpu_enabled           = ui.checkBox_amd_cpu->isChecked();
#if defined(_WIN32)
	info.red_zone_protection_enabled = ui.checkBox_red_zone_protection->isChecked();
#endif
	info.shader_optimization_type = TextToEnum<Configuration::ShaderOptimizationType>(
	    ui.comboBox_shader_optimization_type->currentText());
	info.shader_log_direction = TextToEnum<Configuration::LogDirection>(
	    ui.comboBox_shader_log_direction->currentText());
	info.shader_log_folder           = ui.lineEdit_shader_log_folder->text();
	info.command_buffer_dump_enabled = ui.checkBox_cmd_dump->isChecked();
	info.command_buffer_dump_folder  = ui.lineEdit_cmd_dump_folder->text();
	info.printf_direction =
	    TextToEnum<Configuration::LogDirection>(ui.comboBox_printf_direction->currentText());
	info.printf_output_file = ui.lineEdit_printf_file->text();
	info.profiler_enabled = ui.checkBox_profiler->isChecked();
}

void ConfigurationEditDialog::save() {
	for (auto* field: {m_ui->lineEdit_shader_log_folder, m_ui->lineEdit_printf_file,
	                   m_ui->lineEdit_cmd_dump_folder}) {
		if (field->isEnabled() && field->text().isEmpty()) {
			m_ui->settings_tabs->setCurrentWidget(m_ui->debug_tab);
			field->setFocus();
			break;
		}
	}
	if (MandatoryLineEdit::FindEmpty(this)) {
		QMessageBox::critical(this, tr("Save failed"), tr("Please fill all mandatory fields"));
		return;
	}

	const auto user_name = m_ui->lineEdit_user_name->text().trimmed();
	if (user_name.isEmpty() || user_name.toUtf8().size() > Config::MAX_USER_NAME_LENGTH) {
		QMessageBox::critical(this, tr("Save failed"),
		                      tr("User name must contain 1-16 UTF-8 bytes"));
		return;
	}
	m_ui->lineEdit_user_name->setText(user_name);
	if (!Config::IsConfiguredUserIdValid(m_ui->spinBox_user_id->value())) {
		QMessageBox::critical(this, tr("Save failed"),
		                      tr("User ID cannot be 254 (everyone) or 255 (system)"));
		return;
	}

	UpdateInfo(m_info, *m_ui, m_global_settings);

	emit accept();
}

void ConfigurationEditDialog::clear() {
	Configuration default_info;
	Init(default_info);

	if (m_global_settings) {
		emit PreviewControllerColor({});
		m_game_dirs_list->clear();
		update_game_directory_buttons();
	}
}

void ConfigurationEditDialog::add_game_directory() {
	QString start_dir = QDir::homePath();
	if (m_game_dirs_list->count() > 0) {
		start_dir = m_game_dirs_list->item(m_game_dirs_list->count() - 1)->text();
	}

#if defined(__APPLE__)
	// Use the native macOS picker to browse mounted volumes.
	const auto dir = QFileDialog::getExistingDirectory(this, tr("Select game folder"), start_dir);
	if (dir.isEmpty()) {
		return;
	}
	AddGameDirectoryItem(dir);
#else
	QFileDialog dialog(this, tr("Select game folders"), start_dir);
	dialog.setFileMode(QFileDialog::Directory);
	dialog.setOption(QFileDialog::ShowDirsOnly, true);
	dialog.setOption(QFileDialog::DontUseNativeDialog, true);
	dialog.setLabelText(QFileDialog::Accept, tr("Add"));

	auto* list_view = dialog.findChild<QListView*>(QStringLiteral("listView"));
	if (list_view != nullptr) {
		list_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	}
	auto* tree_view = dialog.findChild<QTreeView*>(QStringLiteral("treeView"));
	if (tree_view != nullptr) {
		tree_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	}

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	for (const auto& dir: dialog.selectedFiles()) {
		AddGameDirectoryItem(dir);
	}
#endif

	update_game_directory_buttons();
}

void ConfigurationEditDialog::remove_selected_game_directories() {
	qDeleteAll(m_game_dirs_list->selectedItems());
	update_game_directory_buttons();
}

void ConfigurationEditDialog::update_game_directory_buttons() {
	m_remove_game_dir_button->setEnabled(!m_game_dirs_list->selectedItems().isEmpty());
}
