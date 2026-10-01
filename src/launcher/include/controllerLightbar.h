#ifndef CONTROLLER_LIGHTBAR_H
#define CONTROLLER_LIGHTBAR_H

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <SDL3/SDL_gamepad.h>

class ControllerLightbar final: public QObject {
public:
	explicit ControllerLightbar(QObject* parent = nullptr);
	~ControllerLightbar() override;

	void SetColor(const QString& color);
	void Stop();

private:
	void Refresh();
	void ApplyColor(SDL_Gamepad* gamepad) const;

	QTimer                              m_timer;
	QHash<SDL_JoystickID, SDL_Gamepad*> m_gamepads;
	QColor                              m_color;
};

#endif // CONTROLLER_LIGHTBAR_H
