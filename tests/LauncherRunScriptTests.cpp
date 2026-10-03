#include "linuxRunScript.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "LauncherRunScriptTests: %s\n", message);
    std::abort();
  }
}

void WriteExecutable(const QString &path, const QByteArray &text) {
  QFile file(path);
  Check(file.open(QIODevice::WriteOnly), "create executable fixture");
  Check(file.write(text) == text.size(), "write executable fixture");
  file.close();
  Check(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner),
        "make fixture executable");
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temp;
  Check(temp.isValid(), "create test directory");

  const auto game_dir = temp.path() + QStringLiteral("/read only's game");
  Check(QDir().mkpath(game_dir), "create game directory");
  const auto emulator = game_dir + QStringLiteral("/emulator's path");
  const auto terminal = temp.path() + QStringLiteral("/fake-terminal");
  const auto actual = temp.path() + QStringLiteral("/actual-argv");

  WriteExecutable(emulator, "#!/usr/bin/env bash\n"
                            "printf '%s\\0' \"$@\" > \"$KYTY_TEST_ARGV\"\n"
                            "exit 7\n");
  WriteExecutable(terminal, "#!/usr/bin/env bash\n"
                            "[[ \"$1\" == \"-e\" ]] || exit 90\n"
                            "shift\n"
                            "bash -c \"$*\"\n");
  Check(QFile::setPermissions(game_dir,
                              QFileDevice::ReadOwner | QFileDevice::ExeOwner),
        "make game directory read-only");
  Check(qputenv("TERMINAL", terminal.toLocal8Bit()), "select fake terminal");

  QTemporaryFile stale(QStringLiteral("/tmp/kytyps5_run_XXXXXX.sh"));
  Check(stale.open(), "create stale script fixture");
  const auto stale_path = stale.fileName();
  Check(stale.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2),
                          QFileDevice::FileModificationTime),
        "age stale script fixture");
  stale.setAutoRemove(false);
  stale.close();

  QProcess process;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("KYTY_TEST_ARGV"), actual);
  process.setProcessEnvironment(environment);
  process.setWorkingDirectory(game_dir);
  const QStringList args{QStringLiteral("argument with spaces"),
                         QStringLiteral("apostrophe's argument"),
                         QStringLiteral("")};
  QString script;
  Check(LinuxRunScript::Prepare(&process, emulator, args, &script),
        "prepare launch");
  Check(!QFile::exists(stale_path), "remove abandoned old script");
  Check(process.program() == terminal, "select fake terminal program");
  Check(process.arguments() ==
            QStringList{QStringLiteral("-e"), QStringLiteral("bash"), script},
        "terminal receives only the script path");
  Check(QFile::exists(script) && !script.startsWith(game_dir),
        "script exists outside read-only game directory");
  Check(script.startsWith(QStringLiteral("/tmp/kytyps5_run_")),
        "terminal receives a path without shell separators");
  const auto permissions = QFileInfo(script).permissions();
  Check((permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                        QFileDevice::ExeGroup | QFileDevice::ReadOther |
                        QFileDevice::WriteOther | QFileDevice::ExeOther)) == 0,
        "temporary script is private to the user");

  QProcess second;
  QString second_script;
  Check(LinuxRunScript::Prepare(&second, emulator, args, &second_script) &&
            second_script != script,
        "concurrent launches use distinct scripts");
  Check(QFile::exists(script),
        "keep current script while preparing another launch");
  Check(QFile::remove(second_script), "remove unused script");

  process.start();
  Check(process.waitForFinished(5000), "fake terminal finished");
  Check(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 7,
        "preserve emulator exit code");
  Check(!QFile::exists(script), "script removed after exit");
  Check(process.readAllStandardOutput().isEmpty(),
        "no prompt without a terminal");

  QFile result(actual);
  Check(result.open(QIODevice::ReadOnly), "open captured arguments");
  QByteArray expected;
  for (const auto &arg : args) {
    expected += arg.toUtf8();
    expected += '\0';
  }
  Check(result.readAll() == expected,
        "preserve spaces, apostrophes, and empty arguments");
  Check(QFile::setPermissions(game_dir, QFileDevice::ReadOwner |
                                            QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner),
        "restore game directory permissions");
  return 0;
}
