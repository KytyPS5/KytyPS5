#include "launcher_qml.h"
#include "mainDialog.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>
#include <QUrl>

void logHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
	static QFile logFile("kyty_launcher.log");
	if (!logFile.isOpen()) {
		logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
	}
	QTextStream stream(&logFile);
	QString     timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
	stream << "[" << timestamp << "] " << msg << "\n";
	stream.flush();
}

int main(int argc, char* argv[]) {
	// Force the "Basic" Qt Quick Controls style. We set the env var instead of
	// using QQuickStyle::setStyle() because the QuickTemplates2 private header
	// may not be installed in every Qt 6 package configuration. Must be set
	// before QApplication is constructed.
	qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

	qInstallMessageHandler(logHandler);
	qDebug() << "=== KytyPS5 Launcher Starting ===";

	QApplication a(argc, argv);
	a.setApplicationName("KytyPS5");
	a.setApplicationVersion("0.1.0");

	QCommandLineParser parser;
	parser.setApplicationDescription("KytyPS5 PlayStation 5 Emulator");
	parser.addHelpOption();
	parser.addVersionOption();

	QCommandLineOption legacyUiOption("legacy-ui", "Use legacy Qt Widgets interface instead of modern QML UI.");
	parser.addOption(legacyUiOption);
	parser.process(a);

	if (parser.isSet(legacyUiOption)) {
		qDebug() << "[KytyPS5] Starting with Legacy Qt Widgets UI...";
		MainDialog* w = new MainDialog();
		QStyle*     s = QStyleFactory::create("fusion");
		QApplication::setStyle(s);
		QObject::connect(&a, &QApplication::aboutToQuit, w, &MainDialog::Quit);
		w->emit Start();
		w->show();
		return QApplication::exec();
	}

	qDebug() << "[KytyPS5] Loading QML Engine interface...";

	qmlRegisterType<GameListModel>("KytyPS5", 1, 0, "GameListModel");
	qmlRegisterType<GameListFilterProxy>("KytyPS5", 1, 0, "GameListFilterProxy");

	QQmlApplicationEngine engine;
	LauncherQML           launcherBridge;

	engine.rootContext()->setContextProperty("launcherBridge", &launcherBridge);

	const QUrl url(QStringLiteral("qrc:/qml/MainAppWindow.qml"));
	QObject::connect(
	    &engine, &QQmlApplicationEngine::objectCreated, &a,
	    [url](QObject* obj, const QUrl& objUrl) {
		    if (!obj && url == objUrl) {
			    qWarning() << "[KytyPS5] Failed to create QML root object from URL:" << objUrl;
		    }
	    },
	    Qt::QueuedConnection);

	engine.load(url);

	if (engine.rootObjects().isEmpty()) {
		qWarning() << "[KytyPS5] Root objects empty after engine load. Falling back to Legacy UI...";
		MainDialog* w = new MainDialog();
		QStyle*     s = QStyleFactory::create("fusion");
		QApplication::setStyle(s);
		QObject::connect(&a, &QApplication::aboutToQuit, w, &MainDialog::Quit);
		w->emit Start();
		w->show();
		return QApplication::exec();
	}

	qDebug() << "[KytyPS5] QML Interface initialized successfully.";
	return QApplication::exec();
}
