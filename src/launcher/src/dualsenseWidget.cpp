#include "dualsenseWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

enum class Shape { Button, Shoulder, Touchpad, Face, Dpad, Stick, Direction };

struct Control {
	const char* id;
	QRectF      bounds;
	Shape       shape = Shape::Button;
	int         angle = 0;
};

// Painting and hit testing share a canvas with room above the shell for the shoulders.
const QRectF CANVAS(0, -20, 480, 330);

const Control CONTROLS[] = {
    {"L2", {90, -12, 58, 18}, Shape::Shoulder},
    {"R2", {332, -12, 58, 18}, Shape::Shoulder},
    {"L1", {84, 12, 64, 12}, Shape::Shoulder},
    {"R1", {332, 12, 64, 12}, Shape::Shoulder},
    {"TouchPad", {150, 25, 90, 100}, Shape::Touchpad},
    {"TouchPadRight", {240, 25, 90, 100}, Shape::Touchpad},
    {"Options", {330, 60, 9, 20}},
    {"Up", {105, 72, 22, 25}, Shape::Dpad, 0},
    {"Right", {124, 94, 25, 22}, Shape::Dpad, 90},
    {"Down", {105, 113, 22, 25}, Shape::Dpad, 180},
    {"Left", {83, 94, 25, 22}, Shape::Dpad, 270},
    {"Triangle", {353, 67, 26, 26}, Shape::Face},
    {"Circle", {382, 96, 26, 26}, Shape::Face},
    {"Cross", {353, 125, 26, 26}, Shape::Face},
    {"Square", {324, 96, 26, 26}, Shape::Face},
    {"L3", {143, 131, 66, 66}, Shape::Stick},
    {"R3", {271, 131, 66, 66}, Shape::Stick},
    {"LeftStickUp", {143, 131, 66, 66}, Shape::Direction, 0},
    {"LeftStickRight", {143, 131, 66, 66}, Shape::Direction, 90},
    {"LeftStickDown", {143, 131, 66, 66}, Shape::Direction, 180},
    {"LeftStickLeft", {143, 131, 66, 66}, Shape::Direction, 270},
    {"RightStickUp", {271, 131, 66, 66}, Shape::Direction, 0},
    {"RightStickRight", {271, 131, 66, 66}, Shape::Direction, 90},
    {"RightStickDown", {271, 131, 66, 66}, Shape::Direction, 180},
    {"RightStickLeft", {271, 131, 66, 66}, Shape::Direction, 270},
};

QPainterPath TouchpadPath() {
	QPainterPath path;
	path.moveTo(158, 42);
	path.cubicTo(156, 33, 168, 32, 178, 31);
	path.cubicTo(219, 28, 261, 28, 302, 31);
	path.cubicTo(312, 32, 324, 33, 322, 42);
	path.cubicTo(318, 64, 315, 87, 309, 105);
	path.cubicTo(306, 117, 299, 122, 286, 122);
	path.lineTo(194, 122);
	path.cubicTo(181, 122, 174, 117, 171, 105);
	path.cubicTo(165, 87, 162, 64, 158, 42);
	path.closeSubpath();
	return path;
}

QPainterPath ControlPath(const Control& control) {
	QPainterPath path;
	if (control.shape == Shape::Direction) {
		const auto inner = control.bounds.adjusted(7, 7, -7, -7);
		const int  start = 49 - control.angle;
		path.arcMoveTo(control.bounds, start);
		path.arcTo(control.bounds, start, 82);
		path.arcTo(inner, start + 82, -82);
		path.closeSubpath();
	} else if (control.shape == Shape::Touchpad) {
		path.addRect(control.bounds);
		return TouchpadPath().intersected(path);
	} else if (control.shape == Shape::Face || control.shape == Shape::Stick) {
		path.addEllipse(control.shape == Shape::Stick ? control.bounds.adjusted(7, 7, -7, -7)
		                                             : control.bounds);
	} else if (control.shape == Shape::Dpad) {
		path.moveTo(-8, -12);
		path.quadTo(-11, -12, -11, -8);
		path.lineTo(-10, 3);
		path.quadTo(-10, 6, -7, 8);
		path.lineTo(0, 12);
		path.lineTo(7, 8);
		path.quadTo(10, 6, 10, 3);
		path.lineTo(11, -8);
		path.quadTo(11, -12, 8, -12);
		path.closeSubpath();
		QTransform transform;
		transform.translate(control.bounds.center().x(), control.bounds.center().y());
		transform.rotate(control.angle);
		return transform.map(path);
	} else if (control.shape == Shape::Shoulder) {
		const auto& bounds = control.bounds;
		path.moveTo(bounds.bottomLeft());
		path.lineTo(bounds.left() + 2, bounds.top() + bounds.height() / 2);
		path.cubicTo(bounds.left() + 10, bounds.top(), bounds.right() - 10, bounds.top(),
		             bounds.right() - 2, bounds.top() + bounds.height() / 2);
		path.lineTo(bounds.bottomRight());
		path.closeSubpath();
	} else {
		path.addRoundedRect(control.bounds, 4, 4);
	}
	return path;
}

void DrawSymbol(QPainter& painter, const Control& control) {
	const auto id = QLatin1String(control.id);
	painter.save();
	painter.translate(control.bounds.center());
	painter.setBrush(Qt::NoBrush);
	if (control.shape == Shape::Dpad) {
		painter.rotate(control.angle);
		painter.drawPolyline(QPolygonF({{-4, 2}, {0, -2}, {4, 2}}));
	} else if (id == QLatin1String("Triangle")) {
		painter.drawPolygon(QPolygonF({{0, -7}, {7, 5}, {-7, 5}}));
	} else if (id == QLatin1String("Circle")) {
		painter.drawEllipse(QPointF(), 7, 7);
	} else if (id == QLatin1String("Square")) {
		painter.drawRect(QRectF(-6, -6, 12, 12));
	} else if (id == QLatin1String("Cross")) {
		painter.drawLine(QPointF(-5, -5), QPointF(5, 5));
		painter.drawLine(QPointF(-5, 5), QPointF(5, -5));
	}
	painter.restore();
}

} // namespace

DualSenseWidget::DualSenseWidget(QWidget* parent): QWidget(parent) {
	setMinimumSize(330, 230);
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setAccessibleName(tr("DualSense controller"));
	setAccessibleDescription(tr("Click a control to select its binding. Connected controller input "
	                            "lights up the controls."));
}

QSize DualSenseWidget::sizeHint() const {
	return CANVAS.size().toSize();
}

void DualSenseWidget::SetPressed(const QSet<QString>& pressed) {
	if (m_pressed != pressed) {
		m_pressed = pressed;
		update();
	}
}

void DualSenseWidget::SetSelected(const QString& selected) {
	if (m_selected != selected) {
		m_selected = selected;
		update();
	}
}

void DualSenseWidget::SetSticks(const QPointF& left, const QPointF& right) {
	if (m_left_stick != left || m_right_stick != right) {
		m_left_stick  = left;
		m_right_stick = right;
		update();
	}
}

QTransform DualSenseWidget::ViewTransform() const {
	const qreal scale = std::min(width() / CANVAS.width(), height() / CANVAS.height());
	QTransform  transform;
	transform.translate(width() / 2.0, height() / 2.0);
	transform.scale(scale, scale);
	transform.translate(-CANVAS.center().x(), -CANVAS.center().y());
	return transform;
}

QString DualSenseWidget::HitTest(const QPointF& position) const {
	const auto point = ViewTransform().inverted().map(position);
	for (const auto& control: CONTROLS) {
		if (ControlPath(control).contains(point)) {
			return QString::fromLatin1(control.id);
		}
	}
	return {};
}

void DualSenseWidget::paintEvent(QPaintEvent*) {
	const QColor black("#111111");
	const QColor white("#eeeeee");
	QPainter     painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setTransform(ViewTransform());

	QPainterPath shell;
	shell.moveTo(81, 48);
	shell.cubicTo(151, 21, 329, 21, 399, 48);
	shell.cubicTo(420, 94, 445, 166, 445, 215);
	shell.cubicTo(447, 253, 437, 291, 419, 296);
	shell.cubicTo(403, 302, 395, 297, 389, 283);
	shell.cubicTo(381, 268, 373, 236, 354, 213);
	shell.cubicTo(347, 205, 341, 205, 325, 205);
	shell.lineTo(155, 205);
	shell.cubicTo(139, 205, 133, 205, 126, 213);
	shell.cubicTo(107, 236, 99, 268, 91, 283);
	shell.cubicTo(85, 297, 77, 302, 61, 296);
	shell.cubicTo(43, 291, 33, 253, 35, 215);
	shell.cubicTo(35, 166, 60, 94, 81, 48);
	shell.closeSubpath();
	painter.setBrush(black);
	painter.setPen(QPen(white, 1.5));
	painter.drawPath(shell);

	// Panel seams flow into the inner grips without a second outline across the belly.
	QPainterPath seams;
	seams.moveTo(153, 35);
	seams.cubicTo(153, 69, 176, 105, 158, 127);
	seams.cubicTo(123, 166, 108, 226, 76, 297);
	seams.moveTo(327, 35);
	seams.cubicTo(327, 69, 304, 105, 322, 127);
	seams.cubicTo(357, 166, 372, 226, 404, 297);
	painter.setBrush(Qt::NoBrush);
	painter.drawPath(seams);
	painter.setBrush(black);
	painter.drawPath(TouchpadPath());
	painter.drawRoundedRect(QRectF(141, 60, 9, 20), 4, 4); // Create button, currently unmapped.
	painter.drawEllipse(QPointF(240, 166), 8, 8);
	QFont font = painter.font();
	font.setPixelSize(9);
	font.setBold(true);
	painter.setFont(font);

	for (const auto& control: CONTROLS) {
		const auto id       = QString::fromLatin1(control.id);
		const bool pressed  = m_pressed.contains(id);
		const bool selected = m_selected == id;
		const bool hovered  = m_hovered == id;
		const bool active   = pressed || selected || hovered;
		if ((control.shape == Shape::Direction || control.shape == Shape::Touchpad) && !active) {
			continue;
		}
		const QColor fill = pressed ? QColor("#2eb6df") : active ? QColor("#3a6179") : black;
		painter.setBrush(fill);
		painter.setPen(QPen(active ? QColor("#79e3ff") : white, selected ? 2 : 1.3));
		if (control.shape == Shape::Touchpad) {
			// Clip one shared surface so independent bindings never add a permanent center seam.
			painter.save();
			painter.setClipRect(control.bounds);
			painter.drawPath(TouchpadPath());
			painter.restore();
		} else if (control.shape == Shape::Stick) {
			painter.setBrush(black);
			painter.drawEllipse(control.bounds);
			const auto    position = id == QLatin1String("L3") ? m_left_stick : m_right_stick;
			QPointF       offset(std::clamp(position.x(), -1.0, 1.0),
			                     std::clamp(position.y(), -1.0, 1.0));
			offset *= 5 / std::max(1.0, std::hypot(offset.x(), offset.y()));
			painter.setBrush(fill);
			painter.drawEllipse(control.bounds.adjusted(12, 12, -12, -12).translated(offset));
		} else {
			painter.drawPath(ControlPath(control));
			painter.setPen(QPen(white, 1.3));
			if (control.shape == Shape::Face || control.shape == Shape::Dpad) {
				DrawSymbol(painter, control);
			} else if (control.shape == Shape::Shoulder) {
				painter.drawText(control.bounds.adjusted(0, 2, 0, 0), Qt::AlignCenter, id);
			} else if (id == QLatin1String("Options")) {
				painter.drawLine(control.bounds.center() - QPointF(0, 4),
				                 control.bounds.center() + QPointF(0, 4));
			}
		}
	}
}

void DualSenseWidget::mouseMoveEvent(QMouseEvent* event) {
	const auto hovered = HitTest(event->position());
	if (hovered != m_hovered) {
		m_hovered = hovered;
		setCursor(hovered.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
		setToolTip(hovered);
		update();
	}
}

void DualSenseWidget::mousePressEvent(QMouseEvent* event) {
	if (event->button() == Qt::LeftButton) {
		const auto id = HitTest(event->position());
		if (!id.isEmpty()) {
			emit ControlClicked(id);
			return;
		}
	}
	QWidget::mousePressEvent(event);
}

void DualSenseWidget::leaveEvent(QEvent*) {
	m_hovered.clear();
	unsetCursor();
	update();
}
