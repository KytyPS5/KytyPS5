#ifndef GAME_LIST_TREE_WIDGET_H
#define GAME_LIST_TREE_WIDGET_H

#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPoint>
#include <QResizeEvent>
#include <QString>
#include <QTreeWidget>

class GameListTreeWidget: public QTreeWidget {
public:
	explicit GameListTreeWidget(QWidget* parent = nullptr): QTreeWidget(parent) {
		setAutoFillBackground(false);
		viewport()->setAutoFillBackground(false);
	}

	void SetBackgroundImage(const QString& path) {
		if (m_path == path) {
			return;
		}
		m_path   = path;
		m_source = QPixmap();
		if (!path.isEmpty()) {
			m_source.load(path);
		}
		UpdateScaledBackground();
		viewport()->update();
	}

protected:
	void resizeEvent(QResizeEvent* event) override {
		QTreeWidget::resizeEvent(event);
		UpdateScaledBackground();
	}

	void paintEvent(QPaintEvent* event) override {
		{
			QPainter painter(viewport());
			if (m_scaled.isNull()) {
				painter.fillRect(event->rect(), palette().brush(QPalette::Base));
			} else {
				painter.drawPixmap(m_scaled_pos, m_scaled);
				painter.fillRect(event->rect(), QColor(0, 0, 0, 115));
			}
		}

		QTreeWidget::paintEvent(event);
	}

private:
	void UpdateScaledBackground() {
		m_scaled = QPixmap();
		if (m_source.isNull() || viewport()->size().isEmpty()) {
			return;
		}

		m_scaled     = m_source.scaled(viewport()->size(), Qt::KeepAspectRatioByExpanding,
		                               Qt::FastTransformation);
		m_scaled_pos = QPoint((viewport()->width() - m_scaled.width()) / 2,
		                      (viewport()->height() - m_scaled.height()) / 2);
	}

	QString m_path;
	QPixmap m_source;
	QPixmap m_scaled;
	QPoint  m_scaled_pos;
};

#endif // GAME_LIST_TREE_WIDGET_H
