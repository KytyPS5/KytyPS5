#include "launcher_qml.h"

#include "patchesDialog.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QVariantMap>
#include <QFileDialog>
#include <QWindow>

#include <algorithm>

// --- GameListModel Implementation ---

GameListModel::GameListModel(QObject* parent) : QAbstractListModel(parent) {}

int GameListModel::rowCount(const QModelIndex& parent) const {
	if (parent.isValid()) return 0;
	return m_games.count();
}

QVariant GameListModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || index.row() >= m_games.count())
		return QVariant();

	const GameItem& item = m_games[index.row()];
	switch (role) {
		case TitleRole: return item.title;
		case PathRole: return item.path;
		case IconRole: return item.icon;
		case IconRealRole: return item.iconReal;
		case SerialRole: return item.serial;
		default: return QVariant();
	}
}

QHash<int, QByteArray> GameListModel::roleNames() const {
	QHash<int, QByteArray> roles;
	roles[TitleRole] = "title";
	roles[PathRole] = "path";
	roles[IconRole] = "icon";
	roles[IconRealRole] = "iconReal";
	roles[SerialRole] = "serial";
	return roles;
}

void GameListModel::addGame(const GameItem& game) {
	beginInsertRows(QModelIndex(), m_games.count(), m_games.count());
	m_games.append(game);
	endInsertRows();
}

void GameListModel::clear() {
	beginResetModel();
	m_games.clear();
	endResetModel();
}

// --- GameListFilterProxy Implementation ---

GameListFilterProxy::GameListFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
	setFilterRole(Qt::UserRole + 1); // TitleRole
	setSortCaseSensitivity(Qt::CaseInsensitive);
	setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void GameListFilterProxy::setSearch(const QString& search) {
	if (m_search != search) {
		m_search = search;
		// Use the search text as a substring filter on the title role.
		setFilterFixedString(search);
		emit searchChanged();
	}
}

bool GameListFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
	if (m_search.isEmpty()) return true;
	const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
	if (!idx.isValid()) return false;
	const QString title = idx.data(Qt::UserRole + 1).toString();
	return title.contains(m_search, Qt::CaseInsensitive);
}

// --- LauncherQML Implementation ---

constexpr char CONF_ORG_NAME[] = "Kyty";
constexpr char CONF_APP_NAME[] = "Kyty";

LauncherQML::LauncherQML(QObject* parent) : QObject(parent) {
	m_process = new QProcess(this);
	m_gameModel = new GameListModel(this);
	m_gameFilterModel = new GameListFilterProxy(this);
	m_gameFilterModel->setSourceModel(m_gameModel);
	m_settings = new QSettings(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME, CONF_APP_NAME, this);
	
	connect(m_process, &QProcess::started, this, [this]() {
		emit emulatorStarted();
		qDebug() << "[KytyPS5 Launcher] Emulator process started successfully.";
	});
	
	connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
		emit emulatorFinished();
		qDebug() << "[KytyPS5 Launcher] Emulator process finished with code:" << exitCode;
	});

	loadSettings();
	populateGamesFromSavedDirs();
}

LauncherQML::~LauncherQML() {
	saveSettings();
}

void LauncherQML::loadSettings() {
	m_settings->beginGroup("QML_Launcher");
	m_savedGameDirs = m_settings->value("gameDirs").toStringList();

	// Emulator settings (mirror upstream Configuration).
	m_screenResolution         = m_settings->value("screenResolution",         m_screenResolution).toInt();
	m_vblankFrequency          = m_settings->value("vblankFrequency",          m_vblankFrequency).toInt();
	m_vulkanValidationEnabled  = m_settings->value("vulkanValidationEnabled",  m_vulkanValidationEnabled).toBool();
	m_shaderValidationEnabled  = m_settings->value("shaderValidationEnabled",  m_shaderValidationEnabled).toBool();
	m_commandBufferDumpEnabled = m_settings->value("commandBufferDumpEnabled", m_commandBufferDumpEnabled).toBool();
	m_commandBufferDumpFolder  = m_settings->value("commandBufferDumpFolder",  m_commandBufferDumpFolder).toString();
	m_renderDocEnabled         = m_settings->value("renderDocEnabled",         m_renderDocEnabled).toBool();
	m_nggRectlistDrawEnabled   = m_settings->value("nggRectlistDrawEnabled",   m_nggRectlistDrawEnabled).toBool();
	m_shaderOptimizationType   = m_settings->value("shaderOptimizationType",   m_shaderOptimizationType).toInt();
	m_shaderLogDirection       = m_settings->value("shaderLogDirection",       m_shaderLogDirection).toInt();
	m_shaderLogFolder          = m_settings->value("shaderLogFolder",          m_shaderLogFolder).toString();
	m_printfDirection          = m_settings->value("printfDirection",          m_printfDirection).toInt();
	m_printfOutputFile         = m_settings->value("printfOutputFile",         m_printfOutputFile).toString();
	m_profilerDirection        = m_settings->value("profilerDirection",        m_profilerDirection).toInt();

	// Keyboard / mouse bindings
	m_keyBindings = m_settings->value("keyBindings").toMap();
	m_settings->endGroup();

	// If no bindings were persisted, seed with sensible defaults so the
	// Input Mapping view is populated on first launch.
	if (m_keyBindings.isEmpty()) {
		m_keyBindings = defaultKeyBindings();
	}
}

void LauncherQML::saveSettings() {
	m_settings->beginGroup("QML_Launcher");
	m_settings->setValue("gameDirs", m_savedGameDirs);

	m_settings->setValue("screenResolution",         m_screenResolution);
	m_settings->setValue("vblankFrequency",          m_vblankFrequency);
	m_settings->setValue("vulkanValidationEnabled",  m_vulkanValidationEnabled);
	m_settings->setValue("shaderValidationEnabled",  m_shaderValidationEnabled);
	m_settings->setValue("commandBufferDumpEnabled", m_commandBufferDumpEnabled);
	m_settings->setValue("commandBufferDumpFolder",  m_commandBufferDumpFolder);
	m_settings->setValue("renderDocEnabled",         m_renderDocEnabled);
	m_settings->setValue("nggRectlistDrawEnabled",   m_nggRectlistDrawEnabled);
	m_settings->setValue("shaderOptimizationType",   m_shaderOptimizationType);
	m_settings->setValue("shaderLogDirection",       m_shaderLogDirection);
	m_settings->setValue("shaderLogFolder",          m_shaderLogFolder);
	m_settings->setValue("printfDirection",          m_printfDirection);
	m_settings->setValue("printfOutputFile",         m_printfOutputFile);
	m_settings->setValue("profilerDirection",        m_profilerDirection);

	m_settings->setValue("keyBindings", m_keyBindings);
	m_settings->endGroup();
	m_settings->sync();
}

void LauncherQML::setSelectedGameTitle(const QString& title) {
	if (m_selectedGameTitle != title) {
		m_selectedGameTitle = title;
		emit selectedGameTitleChanged();
	}
}

void LauncherQML::setSelectedGameDesc(const QString& desc) {
	if (m_selectedGameDesc != desc) {
		m_selectedGameDesc = desc;
		emit selectedGameDescChanged();
	}
}

void LauncherQML::setSelectedGameBg(const QString& bg) {
	if (m_selectedGameBg != bg) {
		m_selectedGameBg = bg;
		emit selectedGameBgChanged();
	}
}

void LauncherQML::setSelectedGameIcon(const QString& icon) {
	if (m_selectedGameIcon != icon) {
		m_selectedGameIcon = icon;
		emit selectedGameIconChanged();
	}
}

void LauncherQML::setSelectedGamePath(const QString& path) {
	if (m_selectedGamePath != path) {
		m_selectedGamePath = path;
		emit selectedGamePathChanged();
	}
}

void LauncherQML::setSelectedGameSerial(const QString& serial) {
	if (m_selectedGameSerial != serial) {
		m_selectedGameSerial = serial;
		emit selectedGameSerialChanged();
	}
}

void LauncherQML::setScreenResolution(int val)        { if (m_screenResolution != val)         { m_screenResolution = val;         emit settingsChanged(); saveSettings(); } }
void LauncherQML::setVblankFrequency(int val)         { if (m_vblankFrequency != val)          { m_vblankFrequency = val;          emit settingsChanged(); saveSettings(); } }
void LauncherQML::setVulkanValidationEnabled(bool v) { if (m_vulkanValidationEnabled != v)    { m_vulkanValidationEnabled = v;    emit settingsChanged(); saveSettings(); } }
void LauncherQML::setShaderValidationEnabled(bool v) { if (m_shaderValidationEnabled != v)    { m_shaderValidationEnabled = v;    emit settingsChanged(); saveSettings(); } }
void LauncherQML::setCommandBufferDumpEnabled(bool v){ if (m_commandBufferDumpEnabled != v)   { m_commandBufferDumpEnabled = v;   emit settingsChanged(); saveSettings(); } }
void LauncherQML::setCommandBufferDumpFolder(const QString& v){ if (m_commandBufferDumpFolder != v){ m_commandBufferDumpFolder = v; emit settingsChanged(); saveSettings(); } }
void LauncherQML::setRenderDocEnabled(bool v)        { if (m_renderDocEnabled != v)           { m_renderDocEnabled = v;           emit settingsChanged(); saveSettings(); } }
void LauncherQML::setNggRectlistDrawEnabled(bool v) { if (m_nggRectlistDrawEnabled != v)     { m_nggRectlistDrawEnabled = v;     emit settingsChanged(); saveSettings(); } }
void LauncherQML::setShaderOptimizationType(int val) { if (m_shaderOptimizationType != val)   { m_shaderOptimizationType = val;   emit settingsChanged(); saveSettings(); } }
void LauncherQML::setShaderLogDirection(int val)     { if (m_shaderLogDirection != val)       { m_shaderLogDirection = val;       emit settingsChanged(); saveSettings(); } }
void LauncherQML::setShaderLogFolder(const QString& v){ if (m_shaderLogFolder != v)           { m_shaderLogFolder = v;            emit settingsChanged(); saveSettings(); } }
void LauncherQML::setPrintfDirection(int val)        { if (m_printfDirection != val)         { m_printfDirection = val;          emit settingsChanged(); saveSettings(); } }
void LauncherQML::setPrintfOutputFile(const QString& v){ if (m_printfOutputFile != v)        { m_printfOutputFile = v;           emit settingsChanged(); saveSettings(); } }
void LauncherQML::setProfilerDirection(int val)      { if (m_profilerDirection != val)        { m_profilerDirection = val;        emit settingsChanged(); saveSettings(); } }

void LauncherQML::setKeyBindings(const QVariantMap& bindings) {
	if (m_keyBindings != bindings) {
		m_keyBindings = bindings;
		emit keyBindingsChanged();
		saveSettings();
	}
}

QVariantMap LauncherQML::defaultKeyBindings() const {
	// Keep button names aligned with the emulator's Controller::PAD_BUTTON_* set
	// and the QML InputMappingView labels. Values use SDL-friendly key names
	// (the bare names accepted by SDL_GetKeyFromName). Mouse binds use "Mouse:"
	// prefix so the emulator can distinguish them.
	return {
		{"Up", "W"},
		{"Down", "S"},
		{"Left", "A"},
		{"Right", "D"},
		{"Cross", "J"},
		{"Triangle", "I"},
		{"Square", "K"},
		{"Circle", "L"},
		{"L1", "Q"},
		{"R1", "E"},
		{"L2", ""},
		{"R2", ""},
		{"L3", "Left Shift"},
		{"R3", "Left Ctrl"},
		{"Options", "Return"},
		{"TouchPad", "Backspace"},
	};
}

// Keys the emulator host window intercepts for built-in actions and therefore
// cannot be exposed to emulated DualSense buttons via the launcher keymap.
// SDLK_SPACE -> pause toggle, SDLK_ESCAPE -> exit (see GameEventKeyboard).
// Stored here as the SDL-friendly names that keyName() produces, matched in
// a case-insensitive way so "Space" / "space" / "Esc" all reject.
static const QStringList& ReservedKeyNames() {
	static const QStringList kReserved = {
		QStringLiteral("Space"),
		QStringLiteral("Escape"),
		QStringLiteral("Esc"),
		QStringLiteral("Return"),   // SDLK_RETURN triggers Options by default; harmless but confusing.
		QStringLiteral("F1"),       // RenderDoc capture
		QStringLiteral("F11"),      // fullscreen if/when added
	};
	return kReserved;
}

bool LauncherQML::setBinding(const QString& padButtonName, const QString& qtKeyName) {
	QString normalized = qtKeyName.trimmed();

	if (normalized.isEmpty()) {
		// Clear the binding.
		m_keyBindings.remove(padButtonName);
		emit keyBindingsChanged();
		saveSettings();
		return true;
	}

	// Mouse bindings ("Mouse:Left|Right|Middle|MotionX|MotionY") are validated
	// separately and never collide with keyboard keys.
	const bool is_mouse = normalized.startsWith(QStringLiteral("Mouse:"), Qt::CaseInsensitive);

	if (!is_mouse) {
		// Reject host-reserved keys outright.
		for (const QString& r : ReservedKeyNames()) {
			if (normalized.compare(r, Qt::CaseInsensitive) == 0) {
				qWarning() << "[KytyPS5 Launcher] Rejected binding of reserved key"
				           << normalized << "to" << padButtonName;
				return false;
			}
		}
	}

	// Deduplicate: a single host key should map to at most one pad button. If
	// the same key is currently bound to another pad button, remove it there
	// first so the new binding wins. (Mouse binds dedupe among mouse binds too.)
	for (auto it = m_keyBindings.begin(); it != m_keyBindings.end(); ) {
		if (it.key() == padButtonName) {
			++it;
			continue;
		}
		if (it.value().toString().compare(normalized, Qt::CaseInsensitive) == 0) {
			it = m_keyBindings.erase(it);
		} else {
			++it;
		}
	}

	m_keyBindings.insert(padButtonName, normalized);
	emit keyBindingsChanged();
	saveSettings();
	return true;
}

QString LauncherQML::keyName(int qtKey) const {
	// Convert a Qt::Key to an SDL-friendly name string. We intentionally use
	// SDL_GetScancodeFromKey->SDL_GetScancodeName-style behaviour is not needed;
	// instead translate common Qt::Key values to the names SDL_GetKeyFromName
	// accepts. For printable ASCII keys, the name is just the character uppercased.
	auto translate = [](int k) -> QString {
		if (k >= Qt::Key_A && k <= Qt::Key_Z) return QChar(char(k)).toUpper();
		switch (k) {
			case Qt::Key_Return: return QStringLiteral("Return");
			case Qt::Key_Enter:  return QStringLiteral("Return");
			case Qt::Key_Backspace: return QStringLiteral("Backspace");
			case Qt::Key_Tab:    return QStringLiteral("Tab");
			case Qt::Key_Space:  return QStringLiteral("Space");
			case Qt::Key_Shift:  return QStringLiteral("Left Shift");
			case Qt::Key_Control:return QStringLiteral("Left Ctrl");
			case Qt::Key_Alt:    return QStringLiteral("Left Alt");
			case Qt::Key_Escape: return QStringLiteral("Escape");
			case Qt::Key_Up:     return QStringLiteral("Up");
			case Qt::Key_Down:   return QStringLiteral("Down");
			case Qt::Key_Left:   return QStringLiteral("Left");
			case Qt::Key_Right:  return QStringLiteral("Right");
			default: break;
		}
		// Fallback: let Qt give us something readable for display.
		return QKeySequence(k).toString();
	};
	return translate(qtKey);
}

int LauncherQML::keyValue(const QString& qtKeyName) const {
	if (qtKeyName.startsWith("Mouse:", Qt::CaseInsensitive)) {
		return 0; // Mouse binds are not Qt::Key values; only used as markers.
	}
	QString n = qtKeyName.trimmed();
	if (n.size() == 1) {
		QChar c = n[0].toUpper();
		if (c >= 'A' && c <= 'Z') return Qt::Key_A + c.toLatin1() - 'A';
	}
	if (n.compare("Return", Qt::CaseInsensitive) == 0) return Qt::Key_Return;
	if (n.compare("Backspace", Qt::CaseInsensitive) == 0) return Qt::Key_Backspace;
	if (n.compare("Tab", Qt::CaseInsensitive) == 0) return Qt::Key_Tab;
	if (n.compare("Space", Qt::CaseInsensitive) == 0) return Qt::Key_Space;
	if (n.compare("Left Shift", Qt::CaseInsensitive) == 0) return Qt::Key_Shift;
	if (n.compare("Left Ctrl", Qt::CaseInsensitive) == 0) return Qt::Key_Control;
	if (n.compare("Left Alt", Qt::CaseInsensitive) == 0) return Qt::Key_Alt;
	if (n.compare("Escape", Qt::CaseInsensitive) == 0) return Qt::Key_Escape;
	if (n.compare("Up", Qt::CaseInsensitive) == 0) return Qt::Key_Up;
	if (n.compare("Down", Qt::CaseInsensitive) == 0) return Qt::Key_Down;
	if (n.compare("Left", Qt::CaseInsensitive) == 0) return Qt::Key_Left;
	if (n.compare("Right", Qt::CaseInsensitive) == 0) return Qt::Key_Right;
	return QKeySequence(n)[0].key();
}

QString LauncherQML::buildKeymapArgString() const {
	// Serialize as "PadButton=QtKeyName;PadButton=QtKeyName;..."
	QStringList parts;
	for (auto it = m_keyBindings.cbegin(); it != m_keyBindings.cend(); ++it) {
		QString v = it.value().toString().trimmed();
		if (!v.isEmpty()) {
			parts << it.key() + "=" + v;
		}
	}
	return parts.join(';');
}

void LauncherQML::launchGame(const QString& path) {
	if (m_process->state() == QProcess::Running) {
		qWarning() << "[KytyPS5 Launcher] Emulator is already running!";
		return;
	}

	QDir search_dir(QApplication::applicationDirPath());
	// macOS ships an extensionless kyty_emulator (matching the Linux build);
	// only Windows uses the .exe suffix. Treat every non-Linux/macOS host as
	// Windows so the launcher still finds the executable there.
#if defined(__linux__) || defined(__APPLE__)
	QString exeName = "kyty_emulator";
#else
	QString exeName = "kyty_emulator.exe";
#endif
	QString emulatorPath = search_dir.absoluteFilePath(exeName);
	
	if (!QFile::exists(emulatorPath)) {
		search_dir.cdUp();
		emulatorPath = search_dir.absoluteFilePath(exeName);
	}
	
	if (!QFile::exists(emulatorPath)) {
		qWarning() << "[KytyPS5 Launcher] Cannot find emulator executable!";
		emit logMessageReceived("ERROR", "Loader", "Could not locate kyty_emulator.exe");
		return;
	}

	QStringList args;

	// Emulator settings -> CLI args. Mirrors the upstream launcher's
	// mainDialog::BuildArgs(): every value below is read by src/main.cpp on
	// the emulator side, so the Settings view stays in sync with what the
	// emulator actually honors. The --spirv-debug-printf flag is intentionally
	// not exposed here; upstream hard-codes it to "false".
	static const char* kResolutionW[] = {"1280", "1920"};
	static const char* kResolutionH[] = {"720",  "1080"};
	static const char* kDirectionNames[] = {"Silent", "Console", "File"};
	static const char* kProfilerNames[]   = {"None", "Network"};
	static const char* kShaderOptNames[]  = {"None", "Size", "Performance"};

	const int resIdx = qBound(0, m_screenResolution, 1);
	args << "--screen-width"  << kResolutionW[resIdx];
	args << "--screen-height" << kResolutionH[resIdx];

	args << "--vblank-frequency" << QString::number(qBound(30, m_vblankFrequency, 360));
	args << "--vulkan-validation"          << (m_vulkanValidationEnabled  ? "true" : "false");
	args << "--shader-validation"          << (m_shaderValidationEnabled  ? "true" : "false");
	args << "--shader-optimization-type"    << kShaderOptNames[qBound(0, m_shaderOptimizationType, 2)];
	args << "--shader-log-direction"        << kDirectionNames[qBound(0, m_shaderLogDirection, 2)];
	args << "--shader-log-folder"           << m_shaderLogFolder;
	args << "--command-buffer-dump"         << (m_commandBufferDumpEnabled ? "true" : "false");
	args << "--command-buffer-dump-folder"  << m_commandBufferDumpFolder;
	args << "--printf-direction"            << kDirectionNames[qBound(0, m_printfDirection, 2)];
	args << "--printf-output-file"          << m_printfOutputFile;
	args << "--profiler-direction"          << kProfilerNames[qBound(0, m_profilerDirection, 1)];
	args << "--ngg-rectlist-draw"           << (m_nggRectlistDrawEnabled   ? "true" : "false");
	if (m_renderDocEnabled) {
		args << "--rd";
	}

	// Keyboard / mouse mapping arg: serialised as "Pad=Key;Pad=Key;..."
	{
		QString keymap = buildKeymapArgString();
		if (!keymap.isEmpty()) {
			args << "--keymap" << keymap;
		}
	}

	m_process->setProcessEnvironment(QProcessEnvironment::systemEnvironment());

	// Determine the actual game binary (EBOOT.BIN)
	QString gamePath = path;
	QFileInfo fi(path);
	if (fi.isDir()) {
		gamePath = QDir(path).filePath("eboot.bin");
		if (!QFile::exists(gamePath)) {
			gamePath = path; // Fallback to dir
		}
	}

	args << "--game" << gamePath;

	// Resolve any saved patch plan for this title (matches the legacy
	// mainDialog::BuildArgs flow). Local patches are only supported for PPSA
	// titles; PatchPlanPath produces a non-existent path otherwise.
	if (!m_selectedGameSerial.isEmpty() && PatchesDialog::IsSupportedTitleId(m_selectedGameSerial)) {
		const QString patch_plan = PatchesDialog::PatchPlanPath(m_selectedGameSerial);
		if (QFileInfo::exists(patch_plan)) {
			args << "--game-patch" << patch_plan;
		}
	}

	qDebug() << "[KytyPS5 Launcher] Launching:" << emulatorPath << args;
	emit logMessageReceived("INFO", "Loader", QString("Launching %1").arg(gamePath));
	
	m_process->setProgram(emulatorPath);
	m_process->setArguments(args);
	m_process->start();

	// Keep the launcher visible: only the emulator window should come to the
	// foreground. (Previously this emitted requestMinimize(), which hid the
	// launcher whenever a game was launched.)
}

void LauncherQML::stopGame() {
	if (m_process->state() == QProcess::Running) {
		qDebug() << "[KytyPS5 Launcher] Terminating emulator process.";
		m_process->terminate();
		m_process->waitForFinished(3000);
		if (m_process->state() == QProcess::Running) {
			m_process->kill();
		}
	}
}

void LauncherQML::scanLibrary(const QString& folderPath) {
	qDebug() << "[KytyPS5 Launcher] Adding game folder:" << folderPath;

	// Convert URL to local file path if needed (e.g. from FileDialog)
	QString localPath = folderPath;
	if (localPath.startsWith("file:///")) {
		localPath = QUrl(localPath).toLocalFile();
	} else if (localPath.startsWith("file://")) {
		localPath = QUrl(localPath).toLocalFile();
	}

	// Normalize to canonical absolute path with forward slashes for consistency.
	localPath = QDir(localPath).absolutePath();
	localPath = QDir::cleanPath(localPath);

	if (!QDir(localPath).exists()) {
		qWarning() << "[KytyPS5 Launcher] Selected directory does not exist:" << localPath;
		return;
	}

	if (!m_savedGameDirs.contains(localPath, Qt::CaseInsensitive)) {
		m_savedGameDirs.append(localPath);
		saveSettings();
		populateGamesFromSavedDirs();
	}
}

void LauncherQML::openFolderDialog() {
	// Use a non-static QFileDialog instance so we can attach it to the QML
	// top-level window. A parent-less static getExistingDirectory() dialog opens
	// behind the frameless, maximized QML ApplicationWindow and appears to do
	// nothing when clicked, which is the bug we are fixing here.
	auto* dlg = new QFileDialog(nullptr, tr("Select PS5 Game Directory"),
	                             QDir::homePath());
	dlg->setFileMode(QFileDialog::Directory);
	dlg->setOption(QFileDialog::ShowDirsOnly, true);
	dlg->setOption(QFileDialog::DontResolveSymlinks, true);
	dlg->setWindowModality(Qt::ApplicationModal);

	// Attach to the first top-level window (the QML ApplicationWindow) so that
	// the dialog is raised above it and properly blocks input.
	if (!QGuiApplication::topLevelWindows().isEmpty()) {
		if (auto* w = QGuiApplication::topLevelWindows().first()) {
			dlg->winId();
			if (auto* handle = dlg->windowHandle()) {
				handle->setTransientParent(w);
			}
		}
	}

	dlg->setAttribute(Qt::WA_DeleteOnClose);
	connect(dlg, &QDialog::accepted, this, [this, dlg]() {
		const QList<QUrl> urls = dlg->selectedUrls();
		if (!urls.isEmpty() && urls.first().isValid()) {
			scanLibrary(urls.first().toString());
		} else {
			const QStringList files = dlg->selectedFiles();
			if (!files.isEmpty()) {
				scanLibrary(files.first());
			}
		}
	});
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

namespace {

// Mirrors the legacy ConfigurationListWidget::GetGameMetadata reading the PS5
// sce_sys/param.json manifest so the QML launcher can show real game titles
// and serial IDs instead of raw folder names.
struct GameMetadata {
	QString title_name;
	QString title_id;
};

QString JsonString(const QJsonObject& obj, const QString& key) {
	return obj.value(key).toString().trimmed();
}

QString LocalizedTitleName(const QJsonObject& root) {
	const auto localized = root.value(QStringLiteral("localizedParameters")).toObject();
	if (localized.isEmpty()) return {};

	const auto default_lang = JsonString(localized, QStringLiteral("defaultLanguage"));
	if (!default_lang.isEmpty()) {
		const auto t = JsonString(localized.value(default_lang).toObject(),
		                          QStringLiteral("titleName"));
		if (!t.isEmpty()) return t;
	}
	const auto en = JsonString(localized.value(QStringLiteral("en-US")).toObject(),
	                           QStringLiteral("titleName"));
	if (!en.isEmpty()) return en;
	for (auto it = localized.constBegin(); it != localized.constEnd(); ++it) {
		const auto t = JsonString(it.value().toObject(), QStringLiteral("titleName"));
		if (!t.isEmpty()) return t;
	}
	return {};
}

GameMetadata ReadGameMetadata(const QString& param_file, const QString& fallback) {
	GameMetadata ret;
	ret.title_name = fallback;

	QFile f(param_file);
	if (!f.open(QIODevice::ReadOnly)) return ret;

	QJsonParseError err;
	const auto      doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) return ret;

	const auto root  = doc.object();
	const auto title = LocalizedTitleName(root);
	if (!title.isEmpty()) ret.title_name = title;
	ret.title_id = JsonString(root, QStringLiteral("titleId"));
	return ret;
}

// Real cover/icon URL for a game directory: sce_sys/icon0.png -> pic0.png.
// Returns an empty string when no real artwork exists, so the QML layer can
// render a sharpemu-style initials placeholder instead of the app logo.
QString RealGameIconUrl(const QDir& game_dir) {
	const QString icon0 = game_dir.filePath(QStringLiteral("sce_sys/icon0.png"));
	if (QFileInfo::exists(icon0)) return QUrl::fromLocalFile(icon0).toString();
	const QString pic0 = game_dir.filePath(QStringLiteral("sce_sys/pic0.png"));
	if (QFileInfo::exists(pic0))  return QUrl::fromLocalFile(pic0).toString();
	return {};
}

} // namespace

void LauncherQML::populateGamesFromSavedDirs() {
	m_gameModel->clear();

	const QString eboot_name = QStringLiteral("eboot.bin");
	QSet<QString>            found_keys;
	QList<GameListModel::GameItem> found;

	// Recursive BFS over each saved root, registering any subdirectory that
	// contains eboot.bin as a game. Matches the behaviour of the legacy Qt
	// Widgets launcher (ConfigurationListWidget::ScanGameDirectory). Without
	// this, adding a folder like "D:\Extra" (which holds many game subfolders)
	// shows just one bogus "Extra" entry instead of each game inside it.
	for (const QString& root_path : std::as_const(m_savedGameDirs)) {
		QDir root(root_path);
		if (root_path.isEmpty() || !root.exists()) continue;

		// Queue the selected root itself first so that a directory which
		// directly contains eboot.bin (a single-game install) is registered as
		// a game entry. We only descend into subdirectories when the current
		// directory is *not* itself a game -- matches the legacy Qt Widgets
		// launcher (ConfigurationListWidget::ScanGameDirectory) and lets the
		// UI's "Add Game Folder" action work for both per-game folders and
		// umbrella libraries that hold many game subfolders.
		QList<QDir> pending;
		pending.append(root);

		while (!pending.isEmpty()) {
			QDir game_dir = pending.takeFirst();

			if (game_dir.exists(eboot_name)) {
				const QString key = QDir::cleanPath(game_dir.absolutePath());
				if (!key.isEmpty() && !found_keys.contains(key)) {
					found_keys.insert(key);

					const auto meta = ReadGameMetadata(
					    game_dir.filePath(QStringLiteral("sce_sys/param.json")),
					    game_dir.dirName());

					GameListModel::GameItem item;
					item.path     = key;
					item.title    = meta.title_name;
					item.serial   = meta.title_id.isEmpty() ? QStringLiteral("Unknown") : meta.title_id;
					item.iconReal = RealGameIconUrl(game_dir);
					item.icon     = item.iconReal.isEmpty()
					                ? QStringLiteral("qrc:/icons/logo.png")
					                : item.iconReal;
					found.append(item);
				}
				continue;
			}

			const auto nested =
			    game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
			for (const auto& sd : nested) pending.append(QDir(sd.absoluteFilePath()));
		}
	}

	// Sort by title for stable display.
	std::sort(found.begin(), found.end(),
	          [](const GameListModel::GameItem& a, const GameListModel::GameItem& b) {
		          return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
	          });

	for (const auto& item : found) m_gameModel->addGame(item);
}

void LauncherQML::minimizeLauncher() {
	emit requestMinimize();
}
