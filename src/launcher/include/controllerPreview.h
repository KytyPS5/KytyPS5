#ifndef CONTROLLER_PREVIEW_H
#define CONTROLLER_PREVIEW_H

#include <QColor>
#include <QMap>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QTimer>

#include <SDL3/SDL_gamepad.h>

class ControllerPreview final: public QObject {
	Q_OBJECT

public:
	explicit ControllerPreview(QObject* parent = nullptr);
	~ControllerPreview() override;

	void SetColor(const QString& color);
	void SetPreviewEnabled(bool enabled);
	void Stop();
	void SetSuspended(bool suspended);

	[[nodiscard]] const QSet<QString>& Pressed() const { return m_pressed; }
	[[nodiscard]] QPointF              LeftStick() const { return m_left_stick; }
	[[nodiscard]] QPointF              RightStick() const { return m_right_stick; }
	[[nodiscard]] const QString&       DeviceName() const { return m_device_name; }
	[[nodiscard]] const QString&       Error() const { return m_error; }

signals:
	void StateChanged();

private:
	void UpdateRunning();
	void OpenGamepad(SDL_JoystickID id);
	void Refresh();
	void ApplyColor(SDL_Gamepad* gamepad) const;

	QTimer                             m_timer;
	QMap<SDL_JoystickID, SDL_Gamepad*> m_gamepads;
	QColor                             m_color;
	bool                               m_preview_enabled = false;
	bool                               m_suspended       = false;
	QSet<QString>                      m_pressed;
	QPointF                            m_left_stick;
	QPointF                            m_right_stick;
	QString                            m_device_name;
	QString                            m_error;
};

#endif // CONTROLLER_PREVIEW_H
