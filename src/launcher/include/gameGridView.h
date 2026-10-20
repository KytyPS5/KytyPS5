#ifndef GAME_GRID_VIEW_H
#define GAME_GRID_VIEW_H

#include "gameContent.h"
#include "trophyViewerDialog.h"

#include <QFontMetrics>
#include <QLocale>
#include <vector>
#include <functional>
#include <utility>

#include <QApplication>
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

	// A negative percent hides the controller battery readout.
	void SetBattery(int percent, bool charging) {
		if (m_battery != percent || m_charging != charging) {
			m_battery  = percent;
			m_charging = charging;
			viewport()->update();
		}
	}

	void SetRecentTrophies(std::vector<TrophyViewerDialog::RecentTrophy> trophies) {
		m_recent = std::move(trophies);
		viewport()->update();
	}

	void SetShowRecent(bool show) {
		m_show_recent = show;
		viewport()->update();
	}

	void SetShowAllVisible(bool visible) {
		if (m_show_all != visible) {
			m_show_all = visible;
			if (!visible && m_focus == 4) {
				m_focus = 0;
			}
			viewport()->update();
		}
	}

	void SetShowAllHandler(std::function<void()> handler) {
		m_show_all_handler = std::move(handler);
	}

	// The "Show all" tile, drawn after the last visible game.
	QRect ShowAllRect() const {
		const int last = LastVisibleRow();
		if (last < 0) {
			return QRect(Margin, 10, iconSize().width() / 2, iconSize().width() / 2);
		}
		const auto r = TileRect(last);
		const int  s = iconSize().width() / 2;
		return QRect(r.right() + 1 + TileGap, 10, s, s);
	}

	int LastVisibleRow() const {
		for (int row = (model() != nullptr ? model()->rowCount() : 0) - 1; row >= 0; --row) {
			if (!isRowHidden(row)) {
				return row;
			}
		}
		return -1;
	}

	// 0 = small tile, 1 = selected tile; animated.
	qreal TileScale(int row) const {
		return row >= 0 && row < m_scales.size() ? m_scales[row] : 0.0;
	}

	// Staggered pop-in: 0..1+ (overshoots slightly), one tile after another.
	qreal PopScale(int row) const {
		if (!m_intro) {
			return 1.0;
		}
		const qreal t = qBound<qreal>(0.0, (m_intro_clock.elapsed() - qMin(row, 20) * 80) / 380.0, 1.0);
		const qreal c = 1.70158;
		const qreal u = t - 1.0;
		return 1.0 + (c + 1.0) * u * u * u + c * u * u;
	}

	// Angle in degrees of the glow circling the selected tile's outline.
	qreal GlowAngle() const {
		return m_glow_clock.elapsed() * 0.18;
	}

	void PlayIntro() {
		m_intro = true;
		m_intro_clock.start();
		StartAnimation();
		viewport()->update();
	}

	// Controller navigation: 0 left, 1 right, 2 up, 3 down, 4 accept. Down moves focus from the
	// tiles to the Play / info / menu buttons; accept presses the focused button.
	// Global position just under the "more" button, where its menu is opened.
	QPoint MenuAnchor() const {
		return viewport()->mapToGlobal(MoreRect().bottomLeft() + QPoint(0, 6));
	}

	void GamepadNav(int action) {
		// An open popup menu takes the controller input as keys.
		if (auto* popup = QApplication::activePopupWidget(); popup != nullptr) {
			const Qt::Key k = action == 0   ? Qt::Key_Left
			                  : action == 1 ? Qt::Key_Right
			                  : action == 2 ? Qt::Key_Up
			                  : action == 3 ? Qt::Key_Down
			                  : action == 4 ? Qt::Key_Return
			                                : Qt::Key_Escape;
			QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
			QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier);
			QApplication::sendEvent(popup, &press);
			QApplication::sendEvent(popup, &release);
			return;
		}
		GamepadNavGrid(action);
	}

	void GamepadNavGrid(int action) {
		if (m_title.isEmpty()) {
			m_focus = 0;
		}
		const auto key = [this](Qt::Key k) {
			QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
			QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier);
			QApplication::sendEvent(this, &press);
			QApplication::sendEvent(this, &release);
		};
		switch (action) {
			case 0:
				if (m_focus == 4) m_focus = 0;
				else if (m_focus == 0) key(Qt::Key_Left);
				else m_focus = qMax(1, m_focus - 1);
				break;
			case 1:
				if (m_focus == 0) {
					if (m_show_all && currentIndex().isValid() &&
					    currentIndex().row() == LastVisibleRow()) {
						m_focus = 4;
					} else {
						key(Qt::Key_Right);
					}
				} else if (m_focus != 4) {
					m_focus = qMin(3, m_focus + 1);
				}
				break;
			case 2: m_focus = 0; break;
			case 3:
				if (m_focus == 0 && !m_title.isEmpty()) m_focus = 1;
				break;
			case 4: {
				if (m_focus == 4) {
					if (m_show_all_handler) m_show_all_handler();
					break;
				}
				const auto& handler = m_focus == 1 ? m_play_handler
				                      : m_focus == 2 ? m_info_handler
				                      : m_focus == 3 ? m_menu_handler
				                                     : std::function<void()>();
				if (handler) {
					handler();
				}
				break;
			}
			default: break;
		}
		viewport()->update();
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
		if (m_focus != 0) {
			const auto& target = m_focus == 1 ? play : (m_focus == 2 ? info : more);
			painter.setBrush(Qt::NoBrush);
			painter.setPen(QPen(QColor(120, 190, 255), 3));
			const auto ring = target.adjusted(-4, -4, 4, 4);
			painter.drawRoundedRect(ring, ring.height() / 2.0, ring.height() / 2.0);
		}
		painter.restore();
	}

	void PaintRecent(QPainter& painter, const QRect& r) {
		if (m_recent.empty() || !m_show_recent) {
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
		// Cards wrap onto more rows when the viewport is too narrow to fit them all.
		const int avail = r.width() - 2 * Margin;
		const int cols  = qBound(1, (avail + gap) / (200 + gap), count);
		const int width = qBound(120, (avail - (cols - 1) * gap) / cols, 280);
		const int y0    = top + 34;
		for (int i = 0; i < count; ++i) {
			const auto& t = m_recent[i];
			const int   y = y0 + (i / cols) * (height + gap);
			const QRect card(Margin + (i % cols) * (width + gap), y, width, height);
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
		if (event->button() == Qt::LeftButton && m_show_all && ShowAllRect().contains(event->pos())) {
			if (m_show_all_handler) {
				m_show_all_handler();
			}
			return;
		}
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
		if (event->button() == Qt::LeftButton) {
			// Select by the drawn tile position; Qt's own cell layout differs from it.
			const auto index = indexAt(event->pos());
			if (!index.isValid()) {
				return;
			}
			m_focus = 0;
			selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
			return;
		}
		QListView::mousePressEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent* event) override {
		if (event->button() == Qt::LeftButton || !indexAt(event->pos()).isValid()) {
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
		if (m_intro) {
			if (m_intro_clock.elapsed() > 20 * 80 + 450) {
				m_intro = false;
			} else {
				busy = true;
			}
		}
		// The outline glow keeps animating while a game is selected.
		if (selectionModel() != nullptr && !selectionModel()->selectedIndexes().isEmpty()) {
			busy = true;
		}
		UpdateScrollRange();
		viewport()->update();
		if (!busy) {
			m_timer.stop();
			m_prev = QPixmap();
		}
	}

	void showEvent(QShowEvent* event) override {
		QListView::showEvent(event);
		PlayIntro();
	}

	// The drawn tiles are packed with animated sizes, so the scroll range and the scrolling to
	// an item follow TileRect instead of Qt's fixed cell layout.
	int ContentWidth() const {
		int       width = Margin;
		const int rows  = model() != nullptr ? model()->rowCount() : 0;
		for (int row = 0; row < rows; ++row) {
			if (!isRowHidden(row)) {
				width += TileRect(row).width() + TileGap;
			}
		}
		if (m_show_all) {
			width += iconSize().width() / 2 + TileGap;
		}
		return width - TileGap + Margin;
	}

	void UpdateScrollRange() {
		auto* bar = horizontalScrollBar();
		const int max = qMax(0, ContentWidth() - viewport()->width());
		if (bar->maximum() != max || bar->minimum() != 0) {
			bar->setRange(0, max);
		}
		bar->setPageStep(viewport()->width());
	}

	void updateGeometries() override {
		QListView::updateGeometries();
		UpdateScrollRange();
	}

public:
	void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override {
		if (!index.isValid() || index.model() != model() || isRowHidden(index.row())) {
			return;
		}
		UpdateScrollRange();
		const auto rect = TileRect(index.row());
		auto*      bar  = horizontalScrollBar();
		int        value = bar->value();
		if (hint == PositionAtCenter) {
			value += rect.center().x() - viewport()->width() / 2;
		} else if (rect.left() < Margin) {
			value += rect.left() - Margin;
		} else if (rect.right() > viewport()->width() - Margin) {
			value += rect.right() - (viewport()->width() - Margin);
		}
		bar->setValue(qBound(bar->minimum(), value, bar->maximum()));
	}

protected:
	void resizeEvent(QResizeEvent* event) override {
		QListView::resizeEvent(event);
		UpdateScaledBackground();
		UpdateScrollRange();
	}

	void PaintBattery(QPainter& p, const QRect& r) {
		const QRect pill(r.right() - 120 - 14, 12, 120, 28);
		p.save();
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 120));
		p.drawRoundedRect(pill, 14, 14);

		// Gamepad glyph.
		const qreal gx = pill.left() + 12;
		const qreal gy = pill.top() + 7;
		QPainterPath pad;
		pad.addRoundedRect(QRectF(gx + 3, gy + 1, 20, 11), 5, 5);
		pad.addEllipse(QRectF(gx, gy + 5, 9, 9));
		pad.addEllipse(QRectF(gx + 17, gy + 5, 9, 9));
		p.setBrush(QColor(255, 255, 255, 230));
		p.drawPath(pad.simplified());
		p.setBrush(QColor(0, 0, 0, 150));
		p.drawEllipse(QRectF(gx + 6, gy + 4, 3, 3));
		p.drawEllipse(QRectF(gx + 17, gy + 4, 3, 3));

		// Wired and not charging: a cable instead of the battery readout.
		if (m_battery > 100) {
			const QRectF plug(pill.left() + 52, pill.top() + 9, 12, 10);
			p.setPen(QPen(QColor(255, 255, 255, 230), 2));
			p.setBrush(Qt::NoBrush);
			QPainterPath cable;
			cable.moveTo(pill.left() + 40, pill.top() + 14);
			cable.cubicTo(pill.left() + 44, pill.top() + 4, pill.left() + 48, pill.top() + 24,
			              plug.left(), pill.top() + 14);
			p.drawPath(cable);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 255, 255, 230));
			p.drawRoundedRect(plug, 2, 2);
			p.drawRect(QRectF(plug.right(), pill.top() + 11, 5, 2));
			p.drawRect(QRectF(plug.right(), pill.top() + 15, 5, 2));
			p.restore();
			return;
		}

		// Battery glyph.
		const QRectF body(pill.left() + 46, pill.top() + 8, 26, 12);
		p.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(body, 3, 3);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 220));
		p.drawRoundedRect(QRectF(body.right() + 1.5, body.top() + 3, 2.5, 6), 1, 1);
		p.setBrush(m_charging ? QColor(80, 220, 120)
		                      : (m_battery <= 20 ? QColor(240, 80, 80) : QColor(255, 255, 255)));
		p.drawRoundedRect(QRectF(body.left() + 2, body.top() + 2,
		                         (body.width() - 4) * qBound(0, m_battery, 100) / 100.0,
		                         body.height() - 4),
		                  1.5, 1.5);

		if (m_charging) {
			QPainterPath bolt;
			const qreal bx = body.center().x();
			const qreal by = body.center().y();
			bolt.moveTo(bx + 2, by - 5);
			bolt.lineTo(bx - 4, by + 1);
			bolt.lineTo(bx - 0.5, by + 1);
			bolt.lineTo(bx - 2, by + 5);
			bolt.lineTo(bx + 4, by - 1);
			bolt.lineTo(bx + 0.5, by - 1);
			bolt.closeSubpath();
			p.setBrush(QColor(20, 20, 20, 230));
			p.drawPath(bolt);
		}

		auto f = font();
		f.setBold(true);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(QRect(pill.left() + 80, pill.top(), 38, pill.height()),
		           Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1%").arg(m_battery));
		p.restore();
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
			if (m_battery >= 0) {
				PaintBattery(painter, r);
			}
		}

		QListView::paintEvent(event);
		if (m_show_all) {
			PaintShowAll();
		}
	}

	void keyPressEvent(QKeyEvent* event) override {
		const int k = event->key();
		if (m_focus == 4) {
			if (k == Qt::Key_Left || k == Qt::Key_Up) {
				m_focus = 0;
			} else if ((k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space) &&
			           m_show_all_handler) {
				m_show_all_handler();
			}
			viewport()->update();
			return;
		}
		if (k == Qt::Key_Right && m_show_all && currentIndex().isValid() &&
		    currentIndex().row() == LastVisibleRow()) {
			m_focus = 4;
			viewport()->update();
			return;
		}
		QListView::keyPressEvent(event);
	}

	void PaintShowAll() {
		const auto      rect = ShowAllRect();
		QPainter        p(viewport());
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, m_focus == 4 ? 70 : 38));
		p.drawRoundedRect(rect, 14, 14);
		const QPoint c = rect.center() - QPoint(0, 12);
		const int    d = qMax(10, rect.width() / 7);
		p.setBrush(Qt::white);
		for (int dx = -1; dx <= 1; dx += 2) {
			for (int dy = -1; dy <= 1; dy += 2) {
				p.drawRoundedRect(QRect(c.x() + dx * d * 7 / 10 - d / 2,
				                        c.y() + dy * d * 7 / 10 - d / 2, d, d),
				                  3, 3);
			}
		}
		p.setPen(Qt::white);
		p.drawText(QRect(rect.left(), rect.bottom() - 40, rect.width(), 30), Qt::AlignCenter,
		           tr("Show all"));
		if (m_focus == 4) {
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(QColor(120, 190, 255), 3));
			p.drawRoundedRect(rect.adjusted(-3, -3, 3, 3), 16, 16);
		}
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
	int                   m_focus = 0;
	bool                  m_show_all = false;
	bool                  m_show_recent = true;
	std::function<void()> m_show_all_handler;
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
	QElapsedTimer         m_intro_clock;
	QElapsedTimer         m_glow_clock = [] { QElapsedTimer t; t.start(); return t; }();
	bool                  m_intro = false;
	int                   m_battery  = -1;
	bool                  m_charging = false;
};

#endif // GAME_GRID_VIEW_H
