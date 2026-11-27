#include "launcherTheme.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QObject>
#include <QPalette>
#include <QTimer>

namespace {

QString LoadStyleSheet(const QApplication& application) {
	QFile style_sheet_file(QStringLiteral(":/styles/launcher.qss"));
	if (!style_sheet_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return {};
	}

	auto selection = application.palette().color(QPalette::Highlight);
	selection.setAlpha(110);
	return QString::fromUtf8(style_sheet_file.readAll())
	    .replace(QStringLiteral("@selection"), selection.name(QColor::HexArgb));
}

class PaletteChangeFilter final: public QObject {
public:
	explicit PaletteChangeFilter(QApplication& application)
	    : QObject(&application), m_application(application) {}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (event->type() == QEvent::ApplicationPaletteChange && !m_refresh_pending) {
			m_refresh_pending = true;
			QTimer::singleShot(0, &m_application, [this] {
				m_application.setStyleSheet(LoadStyleSheet(m_application));
				m_refresh_pending = false;
			});
		}

		return QObject::eventFilter(watched, event);
	}

private:
	QApplication& m_application;
	bool          m_refresh_pending = false;
};

} // namespace

namespace LauncherTheme {

void Initialize(QApplication& application) {
	const auto style_sheet = LoadStyleSheet(application);
	if (!style_sheet.isEmpty()) {
		application.setStyleSheet(style_sheet);
		application.installEventFilter(new PaletteChangeFilter(application));
	}
}
} // namespace LauncherTheme
