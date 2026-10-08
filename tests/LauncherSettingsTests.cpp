#include "configurationListWidget.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

static void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "LauncherSettingsTests: %s\n", message);
    std::exit(1);
  }
}

static void SetColor(QSettings &settings, const QString &color) {
  settings.setValue("GlobalConfiguration/controller_color", color);
  settings.sync();
  Require(settings.status() == QSettings::NoError, "fixture settings could not be saved");
}

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  if (!application.arguments().contains("--child")) {
    QTemporaryDir temporary(QDir::tempPath() + "/kyty portable settings-XXXXXX");
    Require(temporary.isValid(), "temporary directory could not be created");
    const QString executable =
        temporary.filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    Require(QFile::copy(QCoreApplication::applicationFilePath(), executable),
            "test executable could not be copied");
    QProcess child;
    child.setProgram(executable);
    child.setArguments({"--child", "--local"});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_PLUGIN_PATH",
                       QCoreApplication::libraryPaths().join(QDir::listSeparator()));
#ifdef _WIN32
    environment.insert("PATH",
                       QCoreApplication::applicationDirPath() + ';' + environment.value("PATH"));
#endif
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start();
    Require(child.waitForFinished(30000), "isolated test process did not finish");
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 2;
  }

  const QDir root(QCoreApplication::applicationDirPath());
  Require(root.mkdir("working") && QDir::setCurrent(root.filePath("working")),
          "unrelated working directory could not be selected");
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.filePath("user"));
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root.filePath("system"));
#ifdef __linux__
  constexpr auto scope = QSettings::UserScope;
#else
  constexpr auto scope = QSettings::SystemScope;
#endif
  QSettings scoped(QSettings::IniFormat, scope, "Kyty", "Kyty");
  SetColor(scoped, "#112233");
  ConfigurationListWidget widget;
  const auto check = [&](const QString &filename, const QString &color) {
    widget.ReadSettings();
    if (QDir::cleanPath(widget.GetSettingsFile()) != QDir::cleanPath(filename)) {
      std::fprintf(stderr, "expected settings: %s\nactual settings: %s\n", qPrintable(filename),
                   qPrintable(widget.GetSettingsFile()));
    }
    Require(QDir::cleanPath(widget.GetSettingsFile()) == QDir::cleanPath(filename),
            "selected the wrong settings file");
    Require(widget.GetGlobalControllerColor() == color, "read settings from the wrong source");
  };
  check(scoped.fileName(), "#112233");
  QFile unrelated_marker("portable.txt");
  Require(unrelated_marker.open(QIODevice::WriteOnly), "unrelated marker could not be created");
  unrelated_marker.close();
  check(scoped.fileName(), "#112233");

  QSettings legacy(QDir::current().filePath("Kyty.ini"), QSettings::IniFormat);
  SetColor(legacy, "#445566");
  check(legacy.fileName(), "#445566");
  Require(root.mkdir("portable.txt"), "directory marker fixture could not be created");
  check(legacy.fileName(), "#445566");
  Require(root.rmdir("portable.txt"), "directory marker fixture could not be removed");

  QFile marker(root.filePath("portable.txt"));
  Require(marker.open(QIODevice::WriteOnly), "portable marker could not be created");
  marker.close();
  const QString portable_path = root.filePath("Kyty.ini");
  std::puts("Checking portable mode from an unrelated working directory");
  check(portable_path, {});
  widget.WriteSettings();
  Require(QFile::exists(portable_path), "portable settings were not created");

  QSettings portable(portable_path, QSettings::IniFormat);
  SetColor(portable, "#778899");
  check(portable_path, "#778899");
  Require(QDir::setCurrent(root.path()), "working directory could not be changed");
  widget.WriteSettings();
  portable.sync();
  Require(portable.value("GlobalConfiguration/controller_color").toString() == "#778899",
          "portable settings did not survive saving");
  legacy.sync();
  scoped.sync();
  Require(legacy.value("GlobalConfiguration/controller_color").toString() == "#445566" &&
              scoped.value("GlobalConfiguration/controller_color").toString() == "#112233",
          "portable save modified existing nonportable settings");

  Require(marker.remove(), "portable marker could not be removed");
  Require(QDir::setCurrent(root.filePath("working")), "working directory could not be restored");
  check(legacy.fileName(), "#445566");
  std::puts("LauncherSettingsTests: passed");
  return 0;
}
