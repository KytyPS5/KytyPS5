#ifndef DEBUG_PANEL_WINDOW_H
#define DEBUG_PANEL_WINDOW_H

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class DebugClient;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTimer;

// One live view of emulator state. Every kind shares the same shape -- a summary block over a
// sortable, filterable table that re-polls the emulator -- and some add their own tools below it.
class DebugPanelWindow final: public QWidget {
public:
	enum class Kind { Threads, Memory, Audio, Gpu, Imports };

	explicit DebugPanelWindow(Kind kind, QWidget* parent = nullptr);
	~DebugPanelWindow() override = default;

	[[nodiscard]] static QString Title(Kind kind);

private:
	void Refresh();
	void Apply(const QJsonObject& reply);
	void ApplyFilter();
	void SetStatus(const QString& text, bool error);

	// Memory: hex viewer.
	void ReadMemory();
	// 3D / GPU: present a guest render target in place of the scan-out image.
	void SelectRenderTarget(bool restore);

	Kind           m_kind;
	QString        m_command;
	DebugClient*   m_client        = nullptr;
	QTimer*        m_timer         = nullptr;
	QLabel*        m_summary       = nullptr;
	QLineEdit*     m_filter        = nullptr;
	QTableWidget*  m_table         = nullptr;
	QCheckBox*     m_auto          = nullptr;
	QSpinBox*      m_interval      = nullptr;
	QLabel*        m_status        = nullptr;
	QLineEdit*     m_address       = nullptr;
	QSpinBox*      m_read_size     = nullptr;
	QPlainTextEdit* m_hex          = nullptr;
	DebugClient*   m_side_client   = nullptr;
	QStringList    m_columns;

	// CPU %: previous per-thread CPU time, keyed by guest thread id.
	QHash<QString, qint64> m_previous_cpu_ns;
	QElapsedTimer          m_cpu_clock;
	qint64                 m_previous_sample_ns = 0;
};

#endif // DEBUG_PANEL_WINDOW_H
