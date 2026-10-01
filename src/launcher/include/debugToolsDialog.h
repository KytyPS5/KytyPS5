#ifndef DEBUG_TOOLS_DIALOG_H
#define DEBUG_TOOLS_DIALOG_H

#include "debugPanelWindow.h"

#include <QMap>
#include <QPointer>
#include <QWidget>

class DebugClient;
class QLabel;
class QTimer;

// Hub for the emulator debug tools. Each button opens its tool as a separate window, so tools can
// sit side by side or on another monitor; opening one that is already open brings it forward.
class DebugToolsDialog final: public QWidget {
public:
	// Shows the single hub instance, creating it on first use.
	static void ShowHub(QWidget* parent);

private:
	explicit DebugToolsDialog(QWidget* parent);

	void Open(DebugPanelWindow::Kind kind);
	void Poll();

	DebugClient* m_client = nullptr;
	QTimer*      m_timer  = nullptr;
	QLabel*      m_state  = nullptr;

	QMap<int, QPointer<DebugPanelWindow>> m_panels;
};

#endif // DEBUG_TOOLS_DIALOG_H
