#include "mainDialog.h"

#include "common.h"
#include "configuration.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "patchesDialog.h"

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QVariant>
#include <QtCore>

#include "ui_main_dialog.h"

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

class QWidget;

#if defined(_WIN32)
constexpr char EMULATOR_EXE[] = "kyty_emulator.exe";
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif

#if defined(_WIN32)
constexpr char CMD_EXE[] = "cmd.exe";
#elif defined(__linux__)
constexpr char KYTY_BASH_FILE[] = "kyty_run.sh";
#endif
#if defined(_WIN32)
constexpr DWORD CMD_X_CHARS = 175;
constexpr DWORD CMD_Y_CHARS = 1000;
#endif
constexpr char SETTINGS_MAIN_DIALOG[]        = "MainDialog";
constexpr char SETTINGS_MAIN_LAST_GEOMETRY[] = "geometry";

class DetachableProcess: public QProcess {
	Q_OBJECT;

public:
	explicit DetachableProcess(QObject* parent = nullptr): QProcess(parent) {}
	void Detach() {
		this->waitForStarted();
		setProcessState(QProcess::NotRunning);
	}
};

class MainDialogPrivate: public QObject {
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(MainDialogPrivate);

public:
	explicit MainDialogPrivate(QObject* parent = nullptr): QObject(parent) {}
	~MainDialogPrivate() override;

	void Setup(MainDialog* main_dialog);

	/*slots:*/

	void Update();
	void FindInterpreter();
	void Run();

	[[nodiscard]] const QString& GetInterpreter() const { return m_interpreter; }

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

private:
	static QByteArray g_last_geometry;

	Ui::MainDialog* m_ui          = {nullptr};
	MainDialog*     m_main_dialog = nullptr;
	QString         m_interpreter;

	/*DetachableProcess*/ QProcess m_process;

	ConfigurationItem* m_running_item = nullptr;
};

QByteArray MainDialogPrivate::g_last_geometry;

MainDialog::MainDialog(QWidget* parent): QDialog(parent), m_p(new MainDialogPrivate(this)) {
	m_p->Setup(this);
}

MainDialogPrivate::~MainDialogPrivate() {
	delete m_ui;
}

void MainDialogPrivate::Setup(MainDialog* main_dialog) {
	m_ui = new Ui::MainDialog;
	m_ui->setupUi(main_dialog);

	m_main_dialog = main_dialog;
	m_ui->widget->SetMainDialog(main_dialog);

	main_dialog->setWindowFlags(Qt::Dialog /*| Qt::MSWindowsFixedSizeDialogHint*/);

	connect(main_dialog, &MainDialog::Start, this, &MainDialogPrivate::FindInterpreter,
	        Qt::QueuedConnection);
	connect(m_ui->widget, &ConfigurationListWidget::Select, this, &MainDialogPrivate::Update);
	connect(m_ui->widget, &ConfigurationListWidget::Run, this, &MainDialogPrivate::Run);
	connect(main_dialog, &MainDialog::Resize, [this]() {
		g_last_geometry = m_main_dialog->saveGeometry();
		m_ui->widget->WriteSettings();
	});

	connect(&m_process,
	        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
	        [this](int /*exitCode*/, QProcess::ExitStatus /*exitStatus*/) {
		        if (m_running_item != nullptr) {
			        m_running_item->SetRunning(false);
		        }
		        Update();
	        });

	// connect(main_dialog, &MainDialog::Quit, [=]() { m_process.Detach(); });

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	m_main_dialog->restoreGeometry(g_last_geometry);

	Update();
}

void MainDialogPrivate::FindInterpreter() {
	QDir search_dir(QApplication::applicationDirPath());
	m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);

	if (!QFile::exists(m_interpreter)) {
		search_dir.cdUp();
		m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	}

	bool found = QFile::exists(m_interpreter);

	if (found) {
		m_ui->label_Interpreter->setText(tr("Emulator: ") + m_interpreter);

		QProcess test;
		test.setProgram(m_interpreter);
		test.start();
		test.waitForFinished();

		auto output = QString(test.readAllStandardOutput());
		auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

		if (lines.count() >= 2) {
			m_ui->label_Version->setText(
			    tr("Version: ") + (lines.at(0).startsWith("exe_name") ? lines.at(1) : lines.at(0)));
		} else {
			found = false;
		}
	}

	if (!found) {
		QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't find emulator"));
		QApplication::quit();
		return;
	}

	// Prompt for a game folder when none are configured, but keep the launcher
	// open if the user dismisses the dialog (quitting here can segfault during
	// nested modal shutdown / background compatibility load).
	m_ui->widget->EnsureGameDirectory();

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	Update();
}

static QString BoolArg(bool value) {
	return value ? QStringLiteral("true") : QStringLiteral("false");
}

static QStringList CreateEmulatorArgs(const Configuration& info) {
	QStringList args;
	auto        r = EnumToText(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.fullscreen_enabled) {
		args << "--fullscreen";
	}
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	args << "--console-language" << QString::number(info.console_language);
	args << "--vulkan-validation" << BoolArg(info.vulkan_validation_enabled);
	args << "--shader-validation" << BoolArg(info.shader_validation_enabled);
	args << "--shader-optimization-type" << EnumToText(info.shader_optimization_type);
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	args << "--command-buffer-dump" << BoolArg(info.command_buffer_dump_enabled);
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction" << EnumToText(info.printf_direction);
	args << "--printf-output-file" << info.printf_output_file;
	args << "--profiler-direction" << EnumToText(info.profiler_direction);
	args << "--spirv-debug-printf" << "false";
#if defined(_WIN32)
	if (info.red_zone_protection_enabled) {
		args << "--redzone";
	}
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	if (info.renderdoc_enabled) {
		args << "--rd";
	}

	QString game = info.basedir;
	if (!info.elf.isEmpty()) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;

	const auto patch_plan = PatchesDialog::PatchPlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}

#ifdef __linux__
static QString BashQuote(QString value) {
	value.replace('\'', "'\\''");
	return QStringLiteral("'") + value + QStringLiteral("'");
}

static bool CreateBashScript(const QString& interpreter, const QStringList& args,
                             const QString& file_name) {
	QFile file(file_name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QTextStream s(&file);

		s << "#!/bin/bash\n";
		s << BashQuote(interpreter);
		for (const auto& arg: args) {
			s << " " << BashQuote(arg);
		}
		s << "\n";
		s << "echo Press any key...\n";
		s << "read -n1\n";

		file.close();

		return file.setPermissions(file.permissions() | QFile::ExeUser | QFile::ExeOwner |
		                           QFile::ExeGroup);
	}
	return false;
}

// Find a terminal and its command separator.
static bool FindTerminal(QString* program, QStringList* prefix) {
	struct TerminalSpec {
		const char* executable;
		const char* separator; // nullptr when the command follows immediately
	};

	static const TerminalSpec candidates[] = {
	    {"x-terminal-emulator", "-e"},
	    {"gnome-terminal", "--"},
	    {"konsole", "-e"},
	    {"xfce4-terminal", "-x"},
	    {"mate-terminal", "--"},
	    {"tilix", "-e"},
	    {"alacritty", "-e"},
	    {"kitty", nullptr},
	    {"foot", nullptr},
	    {"wezterm", "-e"},
	    {"urxvt", "-e"},
	    {"xterm", "-e"},
	};

	const auto try_candidate = [program, prefix](const QString& executable, const char* separator) {
		const auto resolved = QStandardPaths::findExecutable(executable);
		if (resolved.isEmpty()) {
			return false;
		}
		*program = resolved;
		prefix->clear();
		if (separator != nullptr) {
			*prefix << QString::fromLatin1(separator);
		}
		return true;
	};

	if (const auto from_env = qEnvironmentVariable("TERMINAL"); !from_env.isEmpty()) {
		// Reuse the known separator for an explicit terminal.
		const auto  env_name  = QFileInfo(from_env).fileName();
		const char* separator = "-e";
		for (const auto& candidate: candidates) {
			if (env_name == QLatin1String(candidate.executable)) {
				separator = candidate.separator;
				break;
			}
		}
		if (try_candidate(from_env, separator)) {
			return true;
		}
	}

	for (const auto& candidate: candidates) {
		if (try_candidate(QString::fromLatin1(candidate.executable), candidate.separator)) {
			return true;
		}
	}

	return false;
}
#endif

#if defined(_WIN32)
// Quote one token for cmd.exe so paths with spaces survive /K parsing.
static QString WinCmdQuote(QString value) {
	value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
	return QLatin1Char('"') + value + QLatin1Char('"');
}

static QString BuildWinCmdKCommand(const QString& interpreter, const QStringList& args) {
	QString command = WinCmdQuote(QDir::toNativeSeparators(interpreter));
	for (const auto& arg: args) {
		command += QLatin1Char(' ');
		command += WinCmdQuote(arg);
	}
	return command;
}
#endif

void MainDialog::RunInterpreter(QProcess* process, const Configuration& info) {
	const auto& interpreter = m_p->GetInterpreter();

	QFileInfo f(interpreter);
	auto      dir = f.absoluteDir();

	auto args = CreateEmulatorArgs(info);
	if (args.isEmpty()) {
		QMessageBox::critical(this, tr("Error"), tr("Invalid emulator configuration"));
		QApplication::quit();
		return;
	}

#ifdef __linux__
	auto bash_file_name = dir.filePath(KYTY_BASH_FILE);
	if (!CreateBashScript(interpreter, args, bash_file_name)) {
		QMessageBox::critical(this, tr("Error"), tr("Can't create file:\n") + bash_file_name);
		QApplication::quit();
		return;
	}

	{
		QString     terminal;
		QStringList terminal_prefix;
		// Pass the script as a file argument (not bash -c) so paths with spaces work.
		if (FindTerminal(&terminal, &terminal_prefix)) {
			process->setProgram(terminal);
			process->setArguments(terminal_prefix + QStringList {"bash", bash_file_name});
		} else {
			// Run without a terminal as a fallback.
			process->setProgram(QStringLiteral("bash"));
			process->setArguments({bash_file_name});
		}
	}
#elif defined(_WIN32)
	{
		// Use nativeArguments so Qt does not re-quote the /K command string.
		process->setProgram(CMD_EXE);
		process->setArguments({});
		process->setNativeArguments(QStringLiteral("/K \"") +
		                            BuildWinCmdKCommand(interpreter, args) + QLatin1Char('"'));
	}
#else
	process->setProgram(interpreter);
	process->setArguments(args);
#endif
	process->setWorkingDirectory(dir.path());
#if defined(_WIN32)
	process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->flags |= static_cast<uint32_t>(CREATE_NEW_CONSOLE);
		args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USECOUNTCHARS);
		args->startupInfo->dwXCountChars = CMD_X_CHARS;
		args->startupInfo->dwYCountChars = CMD_Y_CHARS;
		// args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USEFILLATTRIBUTE);
		// args->startupInfo->dwFillAttribute =
		//     static_cast<DWORD>(BACKGROUND_BLUE) | static_cast<DWORD>(FOREGROUND_RED) |
		//     static_cast<DWORD>(FOREGROUND_INTENSITY);
	});
#endif
	process->start();
#if !defined(_WIN32)
	// Report immediate launch failures.
	if (!process->waitForStarted(5000)) {
		QMessageBox::critical(
		    this, tr("Error"),
		    tr("Failed to start:\n%1\n\n%2").arg(process->program(), process->errorString()));
		return;
	}
#endif
	process->waitForFinished(100);
}

void MainDialog::WriteSettings(QSettings& s) {
	MainDialogPrivate::WriteSettings(s);
}

void MainDialog::ReadSettings(QSettings& s) {
	MainDialogPrivate::ReadSettings(s);
}

void MainDialog::resizeEvent(QResizeEvent* event) {
	emit Resize();
	QDialog::resizeEvent(event);
}

void MainDialogPrivate::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry);
	}

	s.endGroup();
}

void MainDialogPrivate::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	g_last_geometry = s.value(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry).toByteArray();

	s.endGroup();
}

void MainDialogPrivate::Run() {
	m_running_item = m_ui->widget->GetSelectedItem();
	if (m_running_item == nullptr) {
		return;
	}

	m_running_item->SetRunning(true);

	Configuration info;
	info.CopyFrom(m_running_item->GetInfo());
	info.host_input_mapping = m_ui->widget->GetHostInputMapping();
	m_main_dialog->RunInterpreter(&m_process, info);

	Update();
}

void MainDialogPrivate::Update() {
	const auto* item = m_ui->widget->GetSelectedItem();

	bool run_enabled = (m_process.state() == QProcess::NotRunning && item != nullptr);

	if (run_enabled) {
		const auto& info = item->GetInfo();
		auto        dir  = info.basedir;
		run_enabled      = !dir.isEmpty() && QDir(dir).exists();
	}

	m_ui->widget->SetRunEnabled(run_enabled);
}

#include "mainDialog.moc"
