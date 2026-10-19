#ifndef GAME_GRID_VIEW_H
#define GAME_GRID_VIEW_H

#include "gameContent.h"
#include "trophyViewerDialog.h"

#include <QFontMetrics>
#include <QLocale>
#include <vector>
#include <functional>
#include <utility>

#include <QElapsedTimer>
#include <QFont>
#include <QIcon>
#include <QItemSelection>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QString>
#include <QTimer>
#include <QVector>

// PS5-style carousel view: the selected game's pic0 as full background plus a trophy counter.
class GameGridView: public QListView {
public:
	explicit GameGridView(QWidget* parent = nullptr): QListView(parent) {
		viewport()->setAutoFillBackground(false);
		viewport()->setMouseTracking(true);
		m_timer.setInterval(16);
		connect(&m_timer, &QTimer::timeout, this, [this]() { Tick(); });
	}

	void SetBackgroundImage(const QString& key) {
		if (m_path == key) {
			return;
		}
		m_path     = key;
		m_prev     = m_scaled;
		m_prev_pos = m_scaled_pos;
		m_fade     = m_prev.isNull() ? 1.0 : 0.0;
		m_source   = QPixmap();
		if (!key.isEmpty()) {
			m_source.loadFromData(GameContent::ReadFile(key, QStringLiteral("sce_sys/pic0.png"),
			                                            GameContent::MaxImageSize));
		}
		UpdateScaledBackground();
		if (m_fade < 1.0) {
			StartAnimation();
		}
		viewport()->update();
	}

	// An empty text hides the trophy counter.
	void SetTrophyInfo(const QString& title, const QString& progress, const QString& earned) {
		m_title    = title;
		m_progress = progress;
		m_earned   = earned;
		viewport()->update();
	}

	void SetRecentTrophies(std::vector<TrophyViewerDialog::RecentTrophy> trophies) {
		m_recent = std::move(trophies);
		viewport()->update();
	}

	// 0 = small tile, 1 = selected tile; animated.
	qreal TileScale(int row) const {
		return row >= 0 && row < m_scales.size() ? m_scales[row] : 0.0;
	}

	void SetPlayHandler(std::function<void()> handler) {
		m_play_handler = std::move(handler);
	}

	void SetInfoHandler(std::function<void()> handler) {
		m_info_handler = std::move(handler);
	}

	void SetMenuHandler(std::function<void()> handler) {
		m_menu_handler = std::move(handler);
	}

public:
	static constexpr int TileGap = 40;

	// Tiles are packed left to right with a constant gap, so the enlarged selected tile does not
	// make the spacing uneven.
	QRect TileRect(int row) const {
		const int side = iconSize().width();
		const auto size = [&](int r) {
			const qreal t = TileScale(r);
			return qRound(side / 2.0 + side / 4.0 * (t * t * (3.0 - 2.0 * t)));
		};
		int x = Margin - horizontalScrollBar()->value();
		for (int r = 0; r < row; ++r) {
			if (!isRowHidden(r)) {
				x += size(r) + TileGap;
			}
		}
		const int s = size(row);
		return QRect(x, 10, s, s);
	}

	QModelIndex indexAt(const QPoint& pos) const override {
		const int rows = model() != nullptr ? model()->rowCount() : 0;
		for (int row = 0; row < rows; ++row) {
			if (isRowHidden(row)) {
				continue;
			}
			const auto rect = TileRect(row);
			if (QRect(rect.left() - TileGap / 2, rect.top(), rect.width() + TileGap,
			          rect.height() + 30)
			        .contains(pos)) {
				return model()->index(row, 0);
			}
		}
		return {};
	}

protected:
	static constexpr int Margin = 24;

	int ButtonsTop() const {
		return iconSize().height() * 3 / 4 + 20 + 36;
	}

	QRect PlayRect() const {
		return QRect(Margin, ButtonsTop(), 200, 40);
	}

	QRect InfoRect() const {
		return QRect(Margin + 212, ButtonsTop(), 40, 40);
	}

	QRect MoreRect() const {
		return QRect(Margin + 264, ButtonsTop(), 40, 40);
	}

	void PaintButtons(QPainter& painter) {
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		const auto play = PlayRect();
		const auto info = InfoRect();
		const auto more = MoreRect();
		painter.setPen(Qt::NoPen);
		painter.setBrush(m_hover == 1 ? QColor(255, 255, 255) : QColor(255, 255, 255, 235));
		painter.drawRoundedRect(play, 20, 20);
		for (const auto& [rect, id] : {std::pair {info, 2}, std::pair {more, 3}}) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(0, 0, 0, m_hover == id ? 190 : 130));
			painter.drawEllipse(rect);
			painter.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
			painter.drawEllipse(rect);
		}

		auto font_play = font();
		font_play.setPointSizeF(font().pointSizeF() * 1.15);
		font_play.setBold(true);
		painter.setFont(font_play);
		painter.setPen(QColor(10, 10, 20));
		painter.drawText(play, Qt::AlignCenter, tr("Play"));
		painter.setPen(Qt::white);
		painter.drawText(info, Qt::AlignCenter, QStringLiteral("i"));
		painter.setBrush(Qt::white);
		painter.setPen(Qt::NoPen);
		for (int dx = -9; dx <= 9; dx += 9) {
			painter.drawEllipse(more.center() + QPoint(dx, 0), 2, 2);
		}
		painter.restore();
	}

	void PaintRecent(QPainter& painter, const QRect& r) {
		if (m_recent.empty()) {
			return;
		}
		const int top = ButtonsTop() + 40 + 28;
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		auto heading = font();
		heading.setPointSizeF(font().pointSizeF() * 1.2);
		heading.setBold(true);
		painter.setFont(heading);
		painter.setPen(Qt::white);
		painter.drawText(QRect(Margin, top, 400, 26), Qt::AlignLeft | Qt::AlignVCenter,
		                 tr("Latest activity"));

		constexpr int gap    = 12;
		constexpr int height = 84;
		const int     count  = static_cast<int>(m_recent.size());
		const int     width  = qBound(200, (r.width() - 2 * Margin - (count - 1) * gap) / count, 280);
		const int     y      = top + 34;
		for (int i = 0; i < count; ++i) {
			const auto& t = m_recent[i];
			const QRect card(Margin + i * (width + gap), y, width, height);
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(0, 0, 0, 150));
			painter.drawRoundedRect(card, 14, 14);
			const QRect icon(card.left() + 8, card.top() + 8, 68, 68);
			if (!t.icon.isNull()) {
				painter.drawPixmap(icon, t.icon);
			}
			const int text_x = icon.right() + 10;
			const int text_w = card.right() - text_x - 8;
			auto      name_font = font();
			name_font.setBold(true);
			painter.setFont(name_font);
			painter.setPen(Qt::white);
			const QFontMetrics metrics(name_font);
			painter.drawText(QRect(text_x, card.top() + 8, text_w, 22),
			                 Qt::AlignLeft | Qt::AlignVCenter,
			                 metrics.elidedText(t.name, Qt::ElideRight, text_w));
			if (!t.cup.isNull()) {
				painter.drawPixmap(QRect(text_x, card.top() + 33, 18, 18), t.cup);
			}
			painter.setFont(font());
			painter.drawText(QRect(text_x + 24, card.top() + 31, text_w - 24, 22),
			                 Qt::AlignLeft | Qt::AlignVCenter, t.category);
			painter.setPen(QColor(170, 170, 175));
			painter.drawText(QRect(text_x, card.top() + 54, text_w, 22),
			                 Qt::AlignLeft | Qt::AlignVCenter,
			                 QLocale().toString(t.earned, QLocale::ShortFormat));
		}
		painter.restore();
	}

	void PaintTrophyBox(QPainter& painter, const QRect& r) {
		const QRect box(r.right() - 300, r.bottom() - 90, 280, 70);
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0, 0, 0, 150));
		painter.drawRoundedRect(box, 35, 35);

		// The launcher's own trophy icon, tinted white.
		const QSize icon_size(34, 34);
		auto        cup = QIcon(QStringLiteral(":/icons/trophy.svg")).pixmap(icon_size, devicePixelRatioF());
		QPainter    tint(&cup);
		tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
		tint.fillRect(cup.rect(), Qt::white);
		tint.end();
		painter.drawPixmap(box.left() + 26, box.top() + (box.height() - icon_size.height()) / 2, cup);

		auto small_font = font();
		small_font.setPointSizeF(font().pointSizeF() * 1.1);
		auto big_font = font();
		big_font.setPointSizeF(font().pointSizeF() * 1.6);
		painter.setPen(QColor(170, 170, 175));
		painter.setFont(small_font);
		painter.drawText(QRect(box.left() + 76, box.top() + 6, 100, 22),
		                 Qt::AlignLeft | Qt::AlignVCenter, tr("Progress"));
		painter.drawText(QRect(box.left() + 180, box.top() + 6, 90, 22),
		                 Qt::AlignLeft | Qt::AlignVCenter, tr("Earned"));
		painter.setPen(Qt::white);
		painter.setFont(big_font);
		painter.drawText(QRect(box.left() + 76, box.top() + 30, 100, 34),
		                 Qt::AlignLeft | Qt::AlignVCenter, m_progress);
		painter.drawText(QRect(box.left() + 180, box.top() + 30, 90, 34),
		                 Qt::AlignLeft | Qt::AlignVCenter, m_earned);
		painter.restore();
	}

	bool viewportEvent(QEvent* event) override {
		if (event->type() == QEvent::ToolTip) {
			return true;
		}
		return QListView::viewportEvent(event);
	}

	void mouseMoveEvent(QMouseEvent* event) override {
		const int hover = !m_title.isEmpty() ? (PlayRect().contains(event->pos())   ? 1
		                                        : InfoRect().contains(event->pos()) ? 2
		                                        : MoreRect().contains(event->pos()) ? 3
		                                                                            : 0)
		                                     : 0;
		if (hover != m_hover) {
			m_hover = hover;
			setCursor(hover != 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
			viewport()->update();
		}
		QListView::mouseMoveEvent(event);
	}

	void mousePressEvent(QMouseEvent* event) override {
		if (event->button() == Qt::LeftButton && !m_title.isEmpty()) {
			const auto& handler = PlayRect().contains(event->pos())   ? m_play_handler
			                      : InfoRect().contains(event->pos()) ? m_info_handler
			                      : MoreRect().contains(event->pos()) ? m_menu_handler
			                                                          : std::function<void()>();
			if (PlayRect().contains(event->pos()) || InfoRect().contains(event->pos()) ||
			    MoreRect().contains(event->pos())) {
				if (handler) {
					handler();
				}
				return;
			}
		}
		if (event->button() == Qt::LeftButton && !indexAt(event->pos()).isValid()) {
			return;
		}
		QListView::mousePressEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent* event) override {
		if (!indexAt(event->pos()).isValid()) {
			return;
		}
		QListView::mouseReleaseEvent(event);
	}

	void selectionChanged(const QItemSelection& selected,
	                      const QItemSelection& deselected) override {
		QListView::selectionChanged(selected, deselected);
		StartAnimation();
		// Only picking another game may deselect: put the selection back if it was just emptied.
		if (selectionModel() != nullptr && selectionModel()->selectedIndexes().isEmpty() &&
		    !deselected.indexes().isEmpty()) {
			const QPersistentModelIndex index(deselected.indexes().first());
			QTimer::singleShot(0, this, [this, index] {
				if (index.isValid() && !isRowHidden(index.row()) && selectionModel() != nullptr &&
				    selectionModel()->selectedIndexes().isEmpty() && !m_keep_deselected) {
					selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
				}
			});
		}
	}

	void StartAnimation() {
		if (!m_timer.isActive()) {
			m_clock.start();
			m_timer.start();
		}
	}

	void Tick() {
		const qreal dt   = qMin<qreal>(m_clock.restart(), 50) / 160.0;
		bool        busy = false;
		const int   rows = model() != nullptr ? model()->rowCount() : 0;
		if (m_scales.size() != rows) {
			m_scales.resize(rows);
			m_scales.fill(0.0);
		}
		for (int row = 0; row < rows; ++row) {
			const bool  on     = selectionModel() != nullptr &&
			                    selectionModel()->isSelected(model()->index(row, 0));
			const qreal target = on ? 1.0 : 0.0;
			auto&       value  = m_scales[row];
			if (value < target) {
				value = qMin(target, value + dt);
			} else if (value > target) {
				value = qMax(target, value - dt);
			}
			busy |= value != target;
		}
		if (m_fade < 1.0) {
			m_fade = qMin(1.0, m_fade + dt * 0.6);
			busy   = busy || m_fade < 1.0;
		}
		viewport()->update();
		if (!busy) {
			m_timer.stop();
			m_prev = QPixmap();
		}
	}

	void resizeEvent(QResizeEvent* event) override {
		QListView::resizeEvent(event);
		UpdateScaledBackground();
	}

	void paintEvent(QPaintEvent* event) override {
		{
			QPainter   painter(viewport());
			const auto r = viewport()->rect();
			painter.fillRect(r, QColor(8, 16, 32));
			if (!m_prev.isNull()) {
				painter.setOpacity(1.0 - m_fade);
				painter.drawPixmap(m_prev_pos, m_prev);
			}
			if (!m_scaled.isNull()) {
				painter.setOpacity(m_prev.isNull() ? 1.0 : m_fade);
				painter.drawPixmap(m_scaled_pos, m_scaled);
			}
			painter.setOpacity(1.0);
			if (!m_scaled.isNull() || !m_prev.isNull()) {
				painter.fillRect(r, QColor(0, 0, 0, 90));
			}

			if (!m_title.isEmpty()) {
				PaintButtons(painter);
				PaintRecent(painter, r);
				auto title_font = font();
				title_font.setPointSizeF(font().pointSizeF() * 4.2);
				title_font.setWeight(QFont::Black);
				painter.setPen(Qt::white);
				painter.setFont(title_font);
				painter.drawText(QRect(Margin, r.bottom() - 250, qMin(r.width() - 360, 900), 220),
				                 Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, m_title);
			}

			if (!m_progress.isEmpty()) {
				PaintTrophyBox(painter, r);
			}
		}

		QListView::paintEvent(event);
	}

private:
	void UpdateScaledBackground() {
		m_scaled = QPixmap();
		if (m_source.isNull() || viewport()->size().isEmpty()) {
			return;
		}
		m_scaled     = m_source.scaled(viewport()->size(), Qt::KeepAspectRatioByExpanding,
		                               Qt::SmoothTransformation);
		m_scaled_pos = QPoint((viewport()->width() - m_scaled.width()) / 2,
		                      (viewport()->height() - m_scaled.height()) / 2);
	}

	std::function<void()> m_play_handler;
	std::function<void()> m_info_handler;
	std::function<void()> m_menu_handler;
	int                   m_hover = 0;
	bool                  m_keep_deselected = false;
	QString               m_path;
	QPixmap               m_source;
	QPixmap               m_scaled;
	QPoint                m_scaled_pos;
	QString               m_title;
	QString               m_progress;
	QString               m_earned;
	std::vector<TrophyViewerDialog::RecentTrophy> m_recent;
	QPixmap               m_prev;
	QPoint                m_prev_pos;
	qreal                 m_fade = 1.0;
	QVector<qreal>        m_scales;
	QTimer                m_timer;
	QElapsedTimer         m_clock;
};

#endif // GAME_GRID_VIEW_H
