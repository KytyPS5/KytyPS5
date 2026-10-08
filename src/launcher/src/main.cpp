#include "launcherTheme.h"
#include "mainDialog.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	const QDir   app_dir(QCoreApplication::applicationDirPath());
	if (QFileInfo(app_dir.filePath("portable.txt")).isFile() &&
	    !QDir::setCurrent(app_dir.absolutePath())) {
		QMessageBox::critical(
		    nullptr, QObject::tr("Portable mode"),
		    QObject::tr("Could not use the launcher directory for portable data."));
		return 1;
	}
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
