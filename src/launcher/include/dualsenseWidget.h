#ifndef LAUNCHER_INCLUDE_DUALSENSE_WIDGET_H_
#define LAUNCHER_INCLUDE_DUALSENSE_WIDGET_H_

#include <QPointF>
#include <QSet>
#include <QString>
#include <QTransform>
#include <QWidget>

class DualSenseWidget final: public QWidget {
	Q_OBJECT

public:
	explicit DualSenseWidget(QWidget* parent = nullptr);
	[[nodiscard]] QSize sizeHint() const override;
	void                SetPressed(const QSet<QString>& pressed);
	void                SetSelected(const QString& selected);
	void                SetSticks(const QPointF& left, const QPointF& right);

signals:
	void ControlClicked(const QString& id);

protected:
	void paintEvent(QPaintEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void leaveEvent(QEvent* event) override;

private:
	[[nodiscard]] QTransform ViewTransform() const;
	[[nodiscard]] QString    HitTest(const QPointF& position) const;

	QSet<QString> m_pressed;
	QString       m_selected;
	QString       m_hovered;
	QPointF       m_left_stick;
	QPointF       m_right_stick;
};

#endif /* LAUNCHER_INCLUDE_DUALSENSE_WIDGET_H_ */
