#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QProcess>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QSettings>
#include <QVariantMap>

class GameListModel : public QAbstractListModel {
	Q_OBJECT
public:
	enum GameRoles {
		TitleRole = Qt::UserRole + 1,
		PathRole,
		IconRole,
		IconRealRole,
		SerialRole
	};

	struct GameItem {
		QString title;
		QString path;
		QString icon;       // Fallback-safe URL (app logo when no real cover)
		QString iconReal;   // Real cover URL, or empty when none
		QString serial;
	};

	explicit GameListModel(QObject* parent = nullptr);

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QHash<int, QByteArray> roleNames() const override;

	void addGame(const GameItem& game);
	void clear();

private:
	QList<GameItem> m_games;
};

// Proxy model that filters GameListModel by title (case-insensitive substring).
class GameListFilterProxy : public QSortFilterProxyModel {
	Q_OBJECT
	Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
public:
	explicit GameListFilterProxy(QObject* parent = nullptr);

	[[nodiscard]] QString search() const { return m_search; }
	void setSearch(const QString& search);

signals:
	void searchChanged();

protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
	QString m_search;
};

class LauncherQML : public QObject {
	Q_OBJECT
	Q_PROPERTY(QString selectedGameTitle READ selectedGameTitle WRITE setSelectedGameTitle NOTIFY selectedGameTitleChanged)
	Q_PROPERTY(QString selectedGameDesc READ selectedGameDesc WRITE setSelectedGameDesc NOTIFY selectedGameDescChanged)
	Q_PROPERTY(QString selectedGameBg READ selectedGameBg WRITE setSelectedGameBg NOTIFY selectedGameBgChanged)
	Q_PROPERTY(QString selectedGameIcon READ selectedGameIcon WRITE setSelectedGameIcon NOTIFY selectedGameIconChanged)
	Q_PROPERTY(QString selectedGamePath READ selectedGamePath WRITE setSelectedGamePath NOTIFY selectedGamePathChanged)
	Q_PROPERTY(QString selectedGameSerial READ selectedGameSerial WRITE setSelectedGameSerial NOTIFY selectedGameSerialChanged)

	// Emulator settings. Mirrors the upstream Configuration fields parsed in
	// src/main.cpp and exposed by the legacy ConfigurationEditDialog, so the
	// Settings view only surfaces options the emulator actually honors.
	// Indices for the combobox-style properties follow the upstream enum order.
	Q_PROPERTY(int screenResolution READ screenResolution WRITE setScreenResolution NOTIFY settingsChanged)
	Q_PROPERTY(int vblankFrequency READ vblankFrequency WRITE setVblankFrequency NOTIFY settingsChanged)
	Q_PROPERTY(bool vulkanValidationEnabled READ vulkanValidationEnabled WRITE setVulkanValidationEnabled NOTIFY settingsChanged)
	Q_PROPERTY(bool shaderValidationEnabled READ shaderValidationEnabled WRITE setShaderValidationEnabled NOTIFY settingsChanged)
	Q_PROPERTY(bool commandBufferDumpEnabled READ commandBufferDumpEnabled WRITE setCommandBufferDumpEnabled NOTIFY settingsChanged)
	Q_PROPERTY(QString commandBufferDumpFolder READ commandBufferDumpFolder WRITE setCommandBufferDumpFolder NOTIFY settingsChanged)
	Q_PROPERTY(bool renderDocEnabled READ renderDocEnabled WRITE setRenderDocEnabled NOTIFY settingsChanged)
	Q_PROPERTY(bool nggRectlistDrawEnabled READ nggRectlistDrawEnabled WRITE setNggRectlistDrawEnabled NOTIFY settingsChanged)
	Q_PROPERTY(int shaderOptimizationType READ shaderOptimizationType WRITE setShaderOptimizationType NOTIFY settingsChanged)
	Q_PROPERTY(int shaderLogDirection READ shaderLogDirection WRITE setShaderLogDirection NOTIFY settingsChanged)
	Q_PROPERTY(QString shaderLogFolder READ shaderLogFolder WRITE setShaderLogFolder NOTIFY settingsChanged)
	Q_PROPERTY(int printfDirection READ printfDirection WRITE setPrintfDirection NOTIFY settingsChanged)
	Q_PROPERTY(QString printfOutputFile READ printfOutputFile WRITE setPrintfOutputFile NOTIFY settingsChanged)
	Q_PROPERTY(int profilerDirection READ profilerDirection WRITE setProfilerDirection NOTIFY settingsChanged)

	// Keyboard/mouse input mapping (pad button id -> Qt::Key_Name string)
	Q_PROPERTY(QVariantMap keyBindings READ keyBindings WRITE setKeyBindings NOTIFY keyBindingsChanged)

public:
	explicit LauncherQML(QObject* parent = nullptr);
	~LauncherQML() override;

	[[nodiscard]] QString selectedGameTitle() const { return m_selectedGameTitle; }
	Q_INVOKABLE void setSelectedGameTitle(const QString& title);

	[[nodiscard]] QString selectedGameDesc() const { return m_selectedGameDesc; }
	Q_INVOKABLE void setSelectedGameDesc(const QString& desc);

	[[nodiscard]] QString selectedGameBg() const { return m_selectedGameBg; }
	Q_INVOKABLE void setSelectedGameBg(const QString& bg);

	[[nodiscard]] QString selectedGameIcon() const { return m_selectedGameIcon; }
	Q_INVOKABLE void setSelectedGameIcon(const QString& icon);

	[[nodiscard]] QString selectedGamePath() const { return m_selectedGamePath; }
	Q_INVOKABLE void setSelectedGamePath(const QString& path);

	// Title ID (e.g. "PPSA01234_00") of the currently selected game; carries
	// through to --game-patch via PatchesDialog::PatchPlanPath on launch.
	[[nodiscard]] QString selectedGameSerial() const { return m_selectedGameSerial; }
	Q_INVOKABLE void setSelectedGameSerial(const QString& serial);

	// Display resolution combobox index. 0: 1280x720, 1: 1920x1080.
	// (Mirrors upstream Configuration::Resolution, which only exposes these
	// two presets.)
	[[nodiscard]] int screenResolution() const { return m_screenResolution; }
	void setScreenResolution(int val);

	// Virtual vblank frequency in Hz (upstream --vblank-frequency, 30..360).
	[[nodiscard]] int vblankFrequency() const { return m_vblankFrequency; }
	void setVblankFrequency(int val);

	[[nodiscard]] bool vulkanValidationEnabled() const { return m_vulkanValidationEnabled; }
	void setVulkanValidationEnabled(bool val);

	[[nodiscard]] bool shaderValidationEnabled() const { return m_shaderValidationEnabled; }
	void setShaderValidationEnabled(bool val);

	[[nodiscard]] bool commandBufferDumpEnabled() const { return m_commandBufferDumpEnabled; }
	void setCommandBufferDumpEnabled(bool val);

	[[nodiscard]] QString commandBufferDumpFolder() const { return m_commandBufferDumpFolder; }
	void setCommandBufferDumpFolder(const QString& val);

	[[nodiscard]] bool renderDocEnabled() const { return m_renderDocEnabled; }
	void setRenderDocEnabled(bool val);

	[[nodiscard]] bool nggRectlistDrawEnabled() const { return m_nggRectlistDrawEnabled; }
	void setNggRectlistDrawEnabled(bool val);

	// Shader optimization type: 0: None, 1: Size, 2: Performance
	[[nodiscard]] int shaderOptimizationType() const { return m_shaderOptimizationType; }
	void setShaderOptimizationType(int val);

	// Shader log direction: 0: Silent, 1: Console, 2: File
	[[nodiscard]] int shaderLogDirection() const { return m_shaderLogDirection; }
	void setShaderLogDirection(int val);

	[[nodiscard]] QString shaderLogFolder() const { return m_shaderLogFolder; }
	void setShaderLogFolder(const QString& val);

	// Printf direction: 0: Silent, 1: Console, 2: File
	[[nodiscard]] int printfDirection() const { return m_printfDirection; }
	void setPrintfDirection(int val);

	[[nodiscard]] QString printfOutputFile() const { return m_printfOutputFile; }
	void setPrintfOutputFile(const QString& val);

	// Profiler direction: 0: None, 1: Network
	[[nodiscard]] int profilerDirection() const { return m_profilerDirection; }
	void setProfilerDirection(int val);

	// Keyboard/mouse mapping
	[[nodiscard]] QVariantMap keyBindings() const { return m_keyBindings; }
	void setKeyBindings(const QVariantMap& bindings);

public slots:
	void launchGame(const QString& path);
	void stopGame();
	void scanLibrary(const QString& folderPath);
	Q_INVOKABLE void openFolderDialog();
	GameListModel* getGameModel() { return m_gameModel; }
	GameListFilterProxy* getGameFilterModel() { return m_gameFilterModel; }
	void loadSettings();
	void saveSettings();
	void minimizeLauncher();

	// Keyboard/mouse input mapping helpers.
	// Returns a QVariantMap describing the default DualSense keyboard layout
	// (keys are PS5 pad-button names, values are Qt::Key names as strings).
	Q_INVOKABLE QVariantMap defaultKeyBindings() const;
	// Persist a single binding override (padButtonName -> qtKeyName). An empty
	// qtKeyName clears the binding. Triggers keyBindingsChanged and saveSettings.
	// Returns true if the binding was accepted, false if rejected (e.g. the key
	// is reserved by the emulator's host window, such as Space for pause).
	Q_INVOKABLE bool setBinding(const QString& padButtonName, const QString& qtKeyName);
	// Convert a Qt::Key value (int) to a human-readable key name.
	Q_INVOKABLE QString keyName(int qtKey) const;
	// Convert a Qt::Key name (string) back to the numeric Qt::Key value.
	Q_INVOKABLE int keyValue(const QString& qtKeyName) const;

signals:
	void selectedGameTitleChanged();
	void selectedGameDescChanged();
	void selectedGameBgChanged();
	void selectedGameIconChanged();
	void selectedGamePathChanged();
	void selectedGameSerialChanged();
	void settingsChanged();
	void keyBindingsChanged();
	void logMessageReceived(const QString& level, const QString& module, const QString& message);
	void emulatorStarted();
	void emulatorFinished();
	void requestMinimize();

private:
	QString m_selectedGameTitle{"No game selected"};
	QString m_selectedGameDesc{"Pick a game from the library to view art backdrop."};
	QString m_selectedGameBg;
	QString m_selectedGameIcon;
	QString m_selectedGamePath;
	QString m_selectedGameSerial;

	// Emulator settings (mirror upstream Configuration; see
	// src/launcher/include/configuration.h).
	int  m_screenResolution         = 0;     // 0: 1280x720, 1: 1920x1080
	int  m_vblankFrequency          = 60;
	bool m_vulkanValidationEnabled  = true;
	bool m_shaderValidationEnabled  = true;
	bool m_commandBufferDumpEnabled = false;
	QString m_commandBufferDumpFolder = "_Buffers";
	bool m_renderDocEnabled         = false;
	bool m_nggRectlistDrawEnabled   = true;
	int  m_shaderOptimizationType   = 2;    // 0: None, 1: Size, 2: Performance
	int  m_shaderLogDirection       = 0;    // 0: Silent, 1: Console, 2: File
	QString m_shaderLogFolder       = "_Shaders";
	int  m_printfDirection          = 0;    // 0: Silent, 1: Console, 2: File
	QString m_printfOutputFile      = "_kyty.txt";
	int  m_profilerDirection        = 0;    // 0: None, 1: Network

	// Keyboard/mouse mapping. Keys are PS5 pad button name strings
	// (e.g. "Cross") and values are Qt::Key names (e.g. "Key_J") as produced by
	// QKeyCombination::toString() / QKeySequence.
	QVariantMap m_keyBindings;

	QProcess* m_process = nullptr;
	GameListModel* m_gameModel = nullptr;
	GameListFilterProxy* m_gameFilterModel = nullptr;
	QSettings* m_settings = nullptr;
	QStringList m_savedGameDirs;

	void populateGamesFromSavedDirs();
	QString buildKeymapArgString() const;
};
