#ifndef GAME_VIEWS_H
#define GAME_VIEWS_H

#include "gameContent.h"

#include <QGuiApplication>
#include <QIcon>
#include <QListView>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QString>
#include <QStyledItemDelegate>
#include <QTreeWidget>

inline constexpr int GAME_NAME_COLUMN             = 0;
inline constexpr int GAME_SERIAL_COLUMN           = 1;
inline constexpr int GAME_VERSION_COLUMN          = 2;
inline constexpr int GAME_FIRMWARE_VERSION_COLUMN = 3;
inline constexpr int GAME_SIZE_COLUMN             = 4;
inline constexpr int GAME_PATH_COLUMN             = 5;
inline constexpr int GAME_STATUS_COLUMN           = 6;
inline constexpr int GAME_COMMENT_COLUMN          = 7;

// Holds the transparent list and grid views and paints the selected game's pic0 behind both.
class GameViews: public QStackedWidget {
public:
	using QStackedWidget::QStackedWidget;

	void SetBackgroundImage(const QString& key) {
		if (m_path == key) {
			return;
		}
		m_path   = key;
		m_source = QPixmap();
		if (!key.isEmpty()) {
			m_source.loadFromData(GameContent::ReadFile(key, QStringLiteral("sce_sys/pic0.png"),
			                                            GameContent::MaxImageSize));
		}
		UpdateScaledBackground();
		update();
	}

protected:
	void resizeEvent(QResizeEvent* event) override {
		QStackedWidget::resizeEvent(event);
		UpdateScaledBackground();
	}

	void paintEvent(QPaintEvent* event) override {
		QPainter painter(this);
		auto     overlay = palette().color(QPalette::Base);
		if (!m_scaled.isNull()) {
			painter.drawPixmap((width() - m_scaled.width()) / 2, (height() - m_scaled.height()) / 2,
			                   m_scaled);
			overlay.setAlpha(160);
		}
		painter.fillRect(event->rect(), overlay);
	}

private:
	void UpdateScaledBackground() {
		m_scaled = m_source.isNull() ? QPixmap()
		                             : m_source.scaled(size(), Qt::KeepAspectRatioByExpanding,
		                                               Qt::FastTransformation);
	}

	QString m_path;
	QPixmap m_source;
	QPixmap m_scaled;
};

// A card on a translucent panel, so the text keeps its contrast over the game background.
class GameGridDelegate: public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	QSize sizeHint(const QStyleOptionViewItem& option,
	               const QModelIndex& /*index*/) const override {
		return option.decorationSize +
		       QSize(2 * CARD_PADDING, 3 * CARD_PADDING + 2 * option.fontMetrics.lineSpacing());
	}

	void paint(QPainter* painter, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override {
		// The stylesheet made the view's own palette transparent, so take the panel from the app's.
		auto panel      = QGuiApplication::palette().color(QPalette::Window);
		auto text_color = option.palette.color(QPalette::Text);
		panel.setAlpha(210);
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing);
		painter->setPen(option.state.testFlag(QStyle::State_Selected) ? QPen(text_color)
		                                                              : Qt::NoPen);
		painter->setBrush(panel);
		painter->drawRoundedRect(QRectF(option.rect).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

		const QRect cover(option.rect.topLeft() + QPoint(CARD_PADDING, CARD_PADDING),
		                  option.decorationSize);
		index.data(Qt::DecorationRole).value<QIcon>().paint(painter, cover);

		QRect line(cover.left(), cover.bottom() + 1 + CARD_PADDING, cover.width(),
		           option.fontMetrics.lineSpacing());
		auto  title_font = option.font;
		title_font.setBold(true);
		painter->setFont(title_font);
		painter->setPen(text_color);
		painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
		                  painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight,
		                                                    line.width()));

		line.translate(0, line.height());
		const auto size    = index.siblingAtColumn(GAME_SIZE_COLUMN).data().toString();
		const auto details = QStringLiteral("%1  %2").arg(
		    index.siblingAtColumn(GAME_SERIAL_COLUMN).data().toString(),
		    index.siblingAtColumn(GAME_VERSION_COLUMN).data().toString());
		text_color.setAlpha(200);
		painter->setFont(option.font);
		painter->setPen(text_color);
		painter->drawText(line, Qt::AlignRight | Qt::AlignVCenter, size);
		line.setRight(line.right() - option.fontMetrics.horizontalAdvance(size) - CARD_PADDING);
		painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
		                  option.fontMetrics.elidedText(details, Qt::ElideRight, line.width()));
		painter->restore();
	}

private:
	static constexpr int CARD_PADDING = 8;
};

// Shows the list's games as cards in the grid: both views share the items and the current game.
inline void SetupGameGrid(QTreeWidget* list, QListView* grid) {
	grid->setModel(list->model());
	grid->setSelectionModel(list->selectionModel());
	grid->setViewMode(QListView::IconMode);
	grid->setMovement(QListView::Static);
	grid->setResizeMode(QListView::Adjust);
	grid->setSpacing(8);
	grid->setUniformItemSizes(true);
	grid->setItemDelegate(new GameGridDelegate(grid));
	// A card also shows other columns (the size arrives late), which the grid does not repaint on.
	QObject::connect(list->model(), &QAbstractItemModel::dataChanged, grid->viewport(),
	                 qOverload<>(&QWidget::update));
}

#endif // GAME_VIEWS_H
