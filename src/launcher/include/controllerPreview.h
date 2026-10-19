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
	void SetBatteryMonitor(bool enabled);

	// -1 when no connected controller reports a battery.
	[[nodiscard]] int  BatteryPercent() const { return m_battery_percent; }
	[[nodiscard]] bool BatteryCharging() const { return m_battery_charging; }
	[[nodiscard]] bool ControllerConnected() const { return m_connected; }
	[[nodiscard]] bool ControllerWired() const { return m_wired; }
	[[nodiscard]] const QSet<QString>& Pressed() const { return m_pressed; }
	[[nodiscard]] const QSet<QString>& RawPressed() const { return m_raw_pressed; }
	[[nodiscard]] QPointF              LeftStick() const { return m_left_stick; }
	[[nodiscard]] QPointF              RightStick() const { return m_right_stick; }
	[[nodiscard]] const QString&       DeviceName() const { return m_device_name; }
	[[nodiscard]] const QString&       Error() const { return m_error; }

signals:
	void StateChanged();
	// 0 left, 1 right, 2 up, 3 down, 4 accept (A / cross)
	void NavigationInput(int action);

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
	bool                               m_battery_enabled = false;
	int                                m_battery_percent = -1;
	int                                m_stick_dir       = -1;
	bool                               m_connected       = false;
	bool                               m_wired           = false;
	bool                               m_battery_charging = false;	QSet<QString>                      m_pressed;
	QSet<QString>                      m_raw_pressed;
	QPointF                            m_left_stick;
	QPointF                            m_right_stick;
	QString                            m_device_name;
	QString                            m_error;
};

#endif // CONTROLLER_PREVIEW_H
