#include "mainDialog.h"
#include "launcherTheme.h"
#include "settingsElevation.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);

	// Elevated copy of the configuration file, started by the launcher itself when it cannot save.
	const QStringList args = QCoreApplication::arguments();
	if ((args.size() == 4 || args.size() == 5) &&
	    args.at(1) == QLatin1String(SettingsElevation::COPY_SETTINGS_ARG)) {
		return SettingsElevation::CopySettings(args.at(2), args.at(3),
		                                       args.size() == 5 ? args.at(4) : QString());
	}

	// Title bars and taskbars get the bolder small-size artwork.
	const QIcon small_art(QStringLiteral(":/icons/kyty-small.svg"));
	const QIcon large_art(QStringLiteral(":/icons/kyty.svg"));
	QIcon       icon;
	for (int size: {16, 24, 32, 48, 64}) {
		icon.addPixmap(small_art.pixmap(QSize(size, size), 1.0));
	}
	for (int size: {128, 256}) {
		icon.addPixmap(large_art.pixmap(QSize(size, size), 1.0));
	}
	QApplication::setWindowIcon(icon);
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
