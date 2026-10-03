#include "linuxRunScript.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>

#include <unistd.h>

namespace LinuxRunScript {
namespace {

// /tmp is fixed rather than QDir::tempPath() so the path can't contain spaces.
constexpr char SCRIPT_DIRECTORY[] = "/tmp";

QString BashQuote(QString value) {
	value.replace('\'', "'\\''");
	return QStringLiteral("'") + value + QStringLiteral("'");
}

void RemoveStaleScripts() {
	QDir       temp_dir(QString::fromLatin1(SCRIPT_DIRECTORY));
	const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
	for (const auto& info: temp_dir.entryInfoList({QStringLiteral("kytyps5_run_*.sh")},
	                                              QDir::Files | QDir::NoSymLinks)) {
		if (info.ownerId() == static_cast<uint>(getuid()) && info.lastModified() < cutoff) {
			QFile::remove(info.absoluteFilePath());
		}
	}
}

QString CreateBashScript(const QString& interpreter, const QStringList& args) {
	RemoveStaleScripts();
	QTemporaryFile file(QString::fromLatin1(SCRIPT_DIRECTORY) +
	                    QStringLiteral("/kytyps5_run_XXXXXX.sh"));
	if (!file.open()) {
		return {};
	}

	QTextStream stream(&file);
	stream << "#!/bin/bash\n";
	// Delete this script on exit; the terminal can return before Bash opens it.
	stream << "trap 'rm -f -- \"$0\"' EXIT\n";
	stream << BashQuote(interpreter);
	for (const auto& arg: args) {
		stream << " " << BashQuote(arg);
	}
	stream << "\n";
	stream << "status=$?\n";
	stream << "if [[ -t 0 ]]; then\n";
	stream << "  printf 'Press any key...'\n";
	stream << "  read -r -n 1\n";
	stream << "  printf '\\n'\n";
	stream << "fi\n";
	stream << "exit \"$status\"\n";
	stream.flush();
	if (stream.status() != QTextStream::Ok || !file.flush()) {
		return {};
	}

	const auto file_name = file.fileName();
	file.setAutoRemove(false);
	file.close();
	return file_name;
}

// Find a terminal and its command separator.
bool FindTerminal(QString* program, QStringList* prefix) {
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

} // namespace

bool Prepare(QProcess* process, const QString& interpreter, const QStringList& args,
             QString* script_file) {
	const auto file_name = CreateBashScript(interpreter, args);
	if (file_name.isEmpty()) {
		return false;
	}

	QString     terminal;
	QStringList terminal_prefix;
	if (FindTerminal(&terminal, &terminal_prefix)) {
		process->setProgram(terminal);
		// Pass the script as a file argument (not bash -c) so paths with spaces work.
		process->setArguments(terminal_prefix + QStringList {"bash", file_name});
	} else {
		// Run without a terminal as a fallback.
		process->setProgram(QStringLiteral("bash"));
		process->setArguments({file_name});
	}
	*script_file = file_name;
	return true;
}

} // namespace LinuxRunScript
