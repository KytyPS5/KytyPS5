#include "debugToolsDialog.h"

#include "debugClient.h"

#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

void DebugToolsDialog::ShowHub(QWidget* parent) {
	static QPointer<DebugToolsDialog> hub;
	if (hub == nullptr) {
		hub = new DebugToolsDialog(parent);
	}
	hub->show();
	hub->raise();
	hub->activateWindow();
}

DebugToolsDialog::DebugToolsDialog(QWidget* parent): QWidget(parent, Qt::Window) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Kyty Debug Tools"));

	m_client = new DebugClient(this);
	m_timer  = new QTimer(this);
	m_state  = new QLabel(this);
	m_state->setWordWrap(true);

	struct Tool {
		DebugPanelWindow::Kind kind;
		QString                description;
	};
	const Tool tools[] = {
	    {DebugPanelWindow::Kind::Threads, tr("Guest threads, CPU load per thread")},
	    {DebugPanelWindow::Kind::Memory, tr("Guest memory map, allocations, hex viewer")},
	    {DebugPanelWindow::Kind::Audio, tr("Audio output ports, queue depth, levels")},
	    {DebugPanelWindow::Kind::Gpu, tr("Frame rate, render targets, show a target on screen")},
	    {DebugPanelWindow::Kind::Imports, tr("Unresolved imports the game calls into")},
	};

	auto* grid = new QGridLayout;
	int   row  = 0;
	for (const auto& tool: tools) {
		auto* button = new QPushButton(DebugPanelWindow::Title(tool.kind), this);
		button->setMinimumSize(170, 40);
		auto* text = new QLabel(tool.description, this);
		grid->addWidget(button, row, 0);
		grid->addWidget(text, row, 1);
		connect(button, &QPushButton::clicked, this, [this, kind = tool.kind] { Open(kind); });
		row++;
	}

	auto* hint = new QLabel(
	    tr("Tools read live state from the running game over 127.0.0.1:%1.").arg(KYTY_DEBUG_SERVER_PORT),
	    this);
	hint->setWordWrap(true);
	hint->setForegroundRole(QPalette::PlaceholderText);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(m_state);
	layout->addLayout(grid);
	layout->addWidget(hint);
	layout->addStretch(1);

	connect(m_timer, &QTimer::timeout, this, &DebugToolsDialog::Poll);
	m_timer->start(1000);
	Poll();
}

void DebugToolsDialog::Open(DebugPanelWindow::Kind kind) {
	auto& panel = m_panels[static_cast<int>(kind)];
	if (panel == nullptr) {
		// Parented to the hub's parent, not the hub, so closing the hub leaves open tools alone.
		panel = new DebugPanelWindow(kind, parentWidget());
	}
	panel->show();
	panel->raise();
	panel->activateWindow();
}

void DebugToolsDialog::Poll() {
	if (m_client->Busy()) {
		return;
	}
	m_client->Request(QStringLiteral("ping"), [this](const QJsonObject& reply, const QString& error) {
		if (!error.isEmpty()) {
			m_state->setText(tr("<b>Not connected.</b> Start a game from the launcher; the tools "
			                    "attach automatically."));
			return;
		}
		const auto uptime = static_cast<int>(reply.value(QStringLiteral("uptime")).toDouble());
		m_state->setText(tr("<b>Connected:</b> %1 (%2) - running %3:%4")
		                     .arg(reply.value(QStringLiteral("title")).toString().toHtmlEscaped(),
		                          reply.value(QStringLiteral("title_id")).toString().toHtmlEscaped())
		                     .arg(uptime / 60)
		                     .arg(uptime % 60, 2, 10, QChar('0')));
	});
}
