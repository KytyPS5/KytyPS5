#include "debugPanelWindow.h"

#include "debugClient.h"

#include <QCheckBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Sorts hex addresses and numbers by value rather than as text.
class SortItem final: public QTableWidgetItem {
public:
	explicit SortItem(const QString& text): QTableWidgetItem(text) {}

	bool operator<(const QTableWidgetItem& other) const override {
		bool       a_ok = false;
		bool       b_ok = false;
		const auto a    = Number(text(), &a_ok);
		const auto b    = Number(other.text(), &b_ok);
		if (a_ok && b_ok) {
			return a < b;
		}
		return QString::compare(text(), other.text(), Qt::CaseInsensitive) < 0;
	}

private:
	static double Number(const QString& text, bool* ok) {
		if (text.startsWith(QStringLiteral("0x"))) {
			return static_cast<double>(text.mid(2).toULongLong(ok, 16));
		}
		return text.toDouble(ok);
	}
};

QString CellText(const QJsonValue& value) {
	switch (value.type()) {
		case QJsonValue::Bool: return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
		case QJsonValue::Double: {
			const auto number = value.toDouble();
			if (number == static_cast<double>(static_cast<qint64>(number))) {
				return QString::number(static_cast<qint64>(number));
			}
			return QString::number(number, 'f', 3);
		}
		case QJsonValue::String: return value.toString();
		case QJsonValue::Null:
		case QJsonValue::Undefined: return {};
		default: return QStringLiteral("…");
	}
}

struct KindInfo {
	const char* title;
	const char* command;
	int         interval_ms;
};

KindInfo Info(DebugPanelWindow::Kind kind) {
	switch (kind) {
		case DebugPanelWindow::Kind::Threads: return {"CPU / Threads", "threads", 1000};
		case DebugPanelWindow::Kind::Memory: return {"Memory", "memory", 3000};
		case DebugPanelWindow::Kind::Audio: return {"Audio", "audio", 250};
		case DebugPanelWindow::Kind::Gpu: return {"3D / GPU", "gpu", 1000};
		case DebugPanelWindow::Kind::Imports: return {"Unresolved Imports", "imports", 2000};
	}
	return {"Debug", "ping", 1000};
}

} // namespace

QString DebugPanelWindow::Title(Kind kind) {
	return QString::fromLatin1(Info(kind).title);
}

DebugPanelWindow::DebugPanelWindow(Kind kind, QWidget* parent)
    : QWidget(parent, Qt::Window), m_kind(kind), m_command(QString::fromLatin1(Info(kind).command)) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Kyty Debug - %1").arg(Title(kind)));
	resize(kind == Kind::Memory ? 1100 : 900, kind == Kind::Memory ? 760 : 560);

	m_client = new DebugClient(this);
	m_timer  = new QTimer(this);

	m_summary = new QLabel(this);
	m_summary->setTextFormat(Qt::RichText);
	m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

	m_filter = new QLineEdit(this);
	m_filter->setPlaceholderText(tr("Filter rows"));
	m_filter->setClearButtonEnabled(true);

	m_table = new QTableWidget(this);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setAlternatingRowColors(true);
	m_table->verticalHeader()->setVisible(false);
	m_table->verticalHeader()->setDefaultSectionSize(20);
	m_table->horizontalHeader()->setStretchLastSection(true);
	m_table->setSortingEnabled(true);

	m_auto = new QCheckBox(tr("Auto refresh"), this);
	m_auto->setChecked(true);
	m_interval = new QSpinBox(this);
	m_interval->setRange(100, 60000);
	m_interval->setSingleStep(250);
	m_interval->setSuffix(tr(" ms"));
	m_interval->setValue(Info(kind).interval_ms);
	auto* refresh = new QPushButton(tr("Refresh now"), this);
	m_status      = new QLabel(this);

	auto* controls = new QHBoxLayout;
	controls->addWidget(m_auto);
	controls->addWidget(m_interval);
	controls->addWidget(refresh);
	controls->addWidget(m_status, 1);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(m_summary);
	layout->addWidget(m_filter);

	if (kind == Kind::Memory) {
		// Range table above a hex viewer; double-clicking a range opens it in the viewer.
		m_side_client = new DebugClient(this);
		auto* viewer  = new QWidget(this);
		auto* viewer_layout = new QVBoxLayout(viewer);
		viewer_layout->setContentsMargins(0, 0, 0, 0);
		auto* read_row = new QHBoxLayout;
		m_address      = new QLineEdit(viewer);
		m_address->setPlaceholderText(tr("Address, e.g. 0x900000000"));
		m_read_size = new QSpinBox(viewer);
		m_read_size->setRange(16, 65536);
		m_read_size->setSingleStep(256);
		m_read_size->setValue(512);
		m_read_size->setSuffix(tr(" bytes"));
		auto* read = new QPushButton(tr("Read"), viewer);
		read_row->addWidget(new QLabel(tr("Hex viewer:"), viewer));
		read_row->addWidget(m_address, 1);
		read_row->addWidget(m_read_size);
		read_row->addWidget(read);
		m_hex = new QPlainTextEdit(viewer);
		m_hex->setReadOnly(true);
		m_hex->setLineWrapMode(QPlainTextEdit::NoWrap);
		m_hex->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
		viewer_layout->addLayout(read_row);
		viewer_layout->addWidget(m_hex);

		auto* splitter = new QSplitter(Qt::Vertical, this);
		splitter->addWidget(m_table);
		splitter->addWidget(viewer);
		splitter->setStretchFactor(0, 3);
		splitter->setStretchFactor(1, 2);
		layout->addWidget(splitter, 1);

		connect(read, &QPushButton::clicked, this, &DebugPanelWindow::ReadMemory);
		connect(m_address, &QLineEdit::returnPressed, this, &DebugPanelWindow::ReadMemory);
		connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
			if (auto* item = m_table->item(row, 0); item != nullptr) {
				m_address->setText(item->text());
				ReadMemory();
			}
		});
	} else {
		layout->addWidget(m_table, 1);
	}

	if (kind == Kind::Gpu) {
		auto* gpu_row  = new QHBoxLayout;
		auto* present  = new QPushButton(tr("Show selected render target on screen"), this);
		auto* restore  = new QPushButton(tr("Restore normal output"), this);
		gpu_row->addWidget(present);
		gpu_row->addWidget(restore);
		gpu_row->addStretch(1);
		layout->addLayout(gpu_row);
		m_side_client = new DebugClient(this);
		connect(present, &QPushButton::clicked, this, [this] { SelectRenderTarget(false); });
		connect(restore, &QPushButton::clicked, this, [this] { SelectRenderTarget(true); });
	}

	layout->addLayout(controls);

	connect(m_timer, &QTimer::timeout, this, &DebugPanelWindow::Refresh);
	connect(refresh, &QPushButton::clicked, this, &DebugPanelWindow::Refresh);
	connect(m_auto, &QCheckBox::toggled, this, [this](bool on) {
		if (on) {
			m_timer->start(m_interval->value());
		} else {
			m_timer->stop();
		}
	});
	connect(m_interval, &QSpinBox::valueChanged, this, [this](int ms) {
		if (m_timer->isActive()) {
			m_timer->start(ms);
		}
	});
	connect(m_filter, &QLineEdit::textChanged, this, &DebugPanelWindow::ApplyFilter);

	m_cpu_clock.start();
	m_timer->start(m_interval->value());
	Refresh();
}

void DebugPanelWindow::SetStatus(const QString& text, bool error) {
	m_status->setText(text);
	m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void DebugPanelWindow::Refresh() {
	if (m_client->Busy()) {
		return;
	}
	m_client->Request(m_command, [this](const QJsonObject& reply, const QString& error) {
		if (!error.isEmpty()) {
			SetStatus(error, true);
			return;
		}
		Apply(reply);
		SetStatus(tr("Updated %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))),
		          false);
	});
}

void DebugPanelWindow::Apply(const QJsonObject& reply) {
	// Summary: label/value pairs, three to a row.
	const auto summary = reply.value(QStringLiteral("summary")).toObject();
	QString    html    = QStringLiteral("<table cellspacing='0' cellpadding='3'>");
	int        column  = 0;
	for (auto it = summary.begin(); it != summary.end(); ++it) {
		if (column == 0) {
			html += QStringLiteral("<tr>");
		}
		html += QStringLiteral("<td><b>%1</b></td><td style='padding-right:24px'>%2</td>")
		            .arg(it.key().toHtmlEscaped(), CellText(it.value()).toHtmlEscaped());
		if (++column == 3) {
			html += QStringLiteral("</tr>");
			column = 0;
		}
	}
	m_summary->setText(html + QStringLiteral("</table>"));

	// Columns, with the per-kind derived ones.
	QStringList columns;
	for (const auto& value: reply.value(QStringLiteral("columns")).toArray()) {
		columns << value.toString();
	}
	const int cpu_ns_column = columns.indexOf(QStringLiteral("cpu_ns"));
	const int peak_column   = columns.indexOf(QStringLiteral("peak"));
	auto      shown         = columns;
	if (cpu_ns_column >= 0) {
		shown[cpu_ns_column] = tr("CPU %");
	}
	if (peak_column >= 0) {
		shown[peak_column] = tr("Level");
	}
	if (shown != m_columns) {
		m_columns = shown;
		m_table->setColumnCount(static_cast<int>(shown.size()));
		m_table->setHorizontalHeaderLabels(shown);
	}

	// CPU %: share of one core since the previous sample.
	const qint64 now_ns     = m_cpu_clock.nsecsElapsed();
	const qint64 elapsed_ns = now_ns - m_previous_sample_ns;
	m_previous_sample_ns    = now_ns;
	QHash<QString, qint64> cpu_ns;

	// Keep the selected row and the scroll position across the rebuild.
	QString selected_key;
	if (const auto rows = m_table->selectionModel()->selectedRows(); !rows.isEmpty()) {
		if (auto* item = m_table->item(rows.first().row(), 0); item != nullptr) {
			selected_key = item->text();
		}
	}
	const int scroll = m_table->verticalScrollBar()->value();

	const auto rows = reply.value(QStringLiteral("rows")).toArray();
	m_table->setSortingEnabled(false);
	m_table->setUpdatesEnabled(false);
	if (peak_column >= 0) {
		for (int r = 0; r < m_table->rowCount(); r++) {
			m_table->removeCellWidget(r, peak_column);
		}
	}
	m_table->clearContents();
	m_table->setRowCount(static_cast<int>(rows.size()));
	int select_row = -1;
	for (int r = 0; r < rows.size(); r++) {
		const auto row = rows.at(r).toArray();
		for (int c = 0; c < row.size() && c < m_table->columnCount(); c++) {
			QString text = CellText(row.at(c));
			if (c == cpu_ns_column) {
				const auto key     = CellText(row.at(0));
				const auto current = static_cast<qint64>(row.at(c).toDouble());
				cpu_ns.insert(key, current);
				const auto previous = m_previous_cpu_ns.value(key, -1);
				text = previous >= 0 && elapsed_ns > 0
				           ? QString::number(100.0 * static_cast<double>(current - previous) /
				                                 static_cast<double>(elapsed_ns),
				                             'f', 1)
				           : QStringLiteral("-");
			}
			if (c == peak_column) {
				auto* bar = new QProgressBar(m_table);
				bar->setRange(0, 100);
				bar->setValue(static_cast<int>(row.at(c).toDouble() * 100.0));
				bar->setFormat(QStringLiteral("%p%"));
				m_table->setCellWidget(r, c, bar);
				text = QString::number(row.at(c).toDouble(), 'f', 3);
			}
			m_table->setItem(r, c, new SortItem(text));
		}
		if (!selected_key.isEmpty() && CellText(row.at(0)) == selected_key) {
			select_row = r;
		}
	}
	if (cpu_ns_column >= 0) {
		m_previous_cpu_ns = cpu_ns;
	}
	m_table->setSortingEnabled(true);
	if (select_row >= 0) {
		// Sorting has moved rows; find the key again.
		for (int r = 0; r < m_table->rowCount(); r++) {
			if (auto* item = m_table->item(r, 0); item != nullptr && item->text() == selected_key) {
				m_table->selectRow(r);
				break;
			}
		}
	}
	m_table->verticalScrollBar()->setValue(scroll);
	m_table->setUpdatesEnabled(true);
	if (m_table->property("sized").isNull() && rows.size() > 0) {
		m_table->resizeColumnsToContents();
		if (peak_column >= 0) {
			m_table->setColumnWidth(peak_column, 160);
		}
		m_table->setProperty("sized", true);
	}
	ApplyFilter();
}

void DebugPanelWindow::ApplyFilter() {
	const auto needle = m_filter->text().trimmed();
	for (int r = 0; r < m_table->rowCount(); r++) {
		bool match = needle.isEmpty();
		for (int c = 0; !match && c < m_table->columnCount(); c++) {
			if (auto* item = m_table->item(r, c);
			    item != nullptr && item->text().contains(needle, Qt::CaseInsensitive)) {
				match = true;
			}
		}
		m_table->setRowHidden(r, !match);
	}
}

void DebugPanelWindow::ReadMemory() {
	bool       ok      = false;
	const auto address = m_address->text().trimmed().toULongLong(&ok, 0);
	if (!ok) {
		m_hex->setPlainText(tr("Enter an address such as 0x900000000."));
		return;
	}
	const auto command =
	    QStringLiteral("memread 0x%1 %2").arg(address, 0, 16).arg(m_read_size->value());
	m_side_client->Request(command, [this](const QJsonObject& reply, const QString& error) {
		if (!error.isEmpty()) {
			m_hex->setPlainText(error);
			return;
		}
		const auto base = static_cast<quint64>(reply.value(QStringLiteral("address")).toDouble());
		const auto hex  = reply.value(QStringLiteral("hex")).toString();
		QString    text;
		for (int offset = 0; offset * 2 < hex.size(); offset += 16) {
			QString bytes;
			QString ascii;
			for (int i = 0; i < 16 && (offset + i) * 2 < hex.size(); i++) {
				const auto pair = hex.mid((offset + i) * 2, 2);
				bytes += pair + (i == 7 ? QStringLiteral("  ") : QStringLiteral(" "));
				bool       byte_ok = false;
				const auto value   = pair.toUInt(&byte_ok, 16);
				ascii += byte_ok && value >= 0x20 && value < 0x7f ? QChar(value) : QChar('.');
			}
			text += QStringLiteral("%1  %2 %3\n")
			            .arg(base + static_cast<quint64>(offset), 16, 16, QChar('0'))
			            .arg(bytes, -49)
			            .arg(ascii);
		}
		m_hex->setPlainText(text);
	});
}

void DebugPanelWindow::SelectRenderTarget(bool restore) {
	QString command = QStringLiteral("rtselect 0 0");
	if (!restore) {
		const auto rows = m_table->selectionModel()->selectedRows();
		if (rows.isEmpty()) {
			SetStatus(tr("Select a render target row first"), true);
			return;
		}
		const int  row    = rows.first().row();
		const auto format = m_table->item(row, m_columns.indexOf(QStringLiteral("Format")));
		const auto index  = m_table->item(row, m_columns.indexOf(QStringLiteral("Index")));
		if (format == nullptr || index == nullptr) {
			return;
		}
		command = QStringLiteral("rtselect %1 %2").arg(format->text(), index->text());
	}
	m_side_client->Request(command, [this, restore](const QJsonObject&, const QString& error) {
		SetStatus(error.isEmpty() ? (restore ? tr("Normal output restored")
		                                     : tr("Render target will show within 15 frames"))
		                          : error,
		          !error.isEmpty());
	});
}
