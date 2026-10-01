#include "controllerLightbar.h"

#include <QDebug>

#include <SDL3/SDL.h>

ControllerLightbar::ControllerLightbar(QObject* parent): QObject(parent) {
	m_timer.setInterval(100);
	connect(&m_timer, &QTimer::timeout, this, &ControllerLightbar::Refresh);
}

ControllerLightbar::~ControllerLightbar() {
	Stop();
}

void ControllerLightbar::SetColor(const QString& color) {
	const QColor parsed(color);
	if (!parsed.isValid()) {
		Stop();
		return;
	}
	if (m_color == parsed) {
		return;
	}
	if (!m_timer.isActive()) {
		if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
			qWarning() << "Cannot initialize controller color preview:" << SDL_GetError();
			return;
		}
		m_timer.start();
	}
	m_color = parsed;
	for (auto* gamepad: m_gamepads) {
		ApplyColor(gamepad);
	}
	Refresh();
}

void ControllerLightbar::Stop() {
	if (!m_timer.isActive()) {
		return;
	}
	m_timer.stop();
	for (auto* gamepad: m_gamepads) {
		SDL_CloseGamepad(gamepad);
	}
	m_gamepads.clear();
	SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
	m_color = {};
}

void ControllerLightbar::Refresh() {
	// Bluetooth DualSense may defer an accepted LED command until its connection
	// animation ends. Polling events also delivers those deferred commands.
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_GAMEPAD_ADDED && !m_gamepads.contains(event.gdevice.which)) {
			auto* gamepad = SDL_OpenGamepad(event.gdevice.which);
			if (gamepad == nullptr) {
				qWarning() << "Cannot open controller color preview:" << SDL_GetError();
				continue;
			}
			if (SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad),
			                           SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false)) {
				m_gamepads.insert(event.gdevice.which, gamepad);
				ApplyColor(gamepad);
			} else {
				SDL_CloseGamepad(gamepad);
			}
		} else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
			if (auto* gamepad = m_gamepads.take(event.gdevice.which); gamepad != nullptr) {
				SDL_CloseGamepad(gamepad);
			}
		}
	}
}

void ControllerLightbar::ApplyColor(SDL_Gamepad* gamepad) const {
	if (!SDL_SetGamepadLED(gamepad, static_cast<Uint8>(m_color.red()),
	                       static_cast<Uint8>(m_color.green()),
	                       static_cast<Uint8>(m_color.blue()))) {
		qWarning() << "Cannot set controller LED:" << SDL_GetError();
	}
}
