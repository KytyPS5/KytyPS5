#include "controllerPreview.h"

#include <QDebug>

#include <SDL3/SDL.h>
#include <cmath>

namespace {

constexpr struct {
	SDL_GamepadButton button;
	const char*       control;
} BUTTONS[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, "Cross"},      {SDL_GAMEPAD_BUTTON_EAST, "Circle"},
    {SDL_GAMEPAD_BUTTON_WEST, "Square"},      {SDL_GAMEPAD_BUTTON_NORTH, "Triangle"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, "Up"},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Down"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Left"},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Right"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "L3"},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R3"},
    {SDL_GAMEPAD_BUTTON_START, "Options"},
};

double Axis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) {
	const int value = SDL_GetGamepadAxis(gamepad, axis);
	return value / (value < 0 ? 32768.0 : 32767.0);
}

} // namespace

ControllerPreview::ControllerPreview(QObject* parent): QObject(parent) {
	m_timer.setInterval(100);
	connect(&m_timer, &QTimer::timeout, this, &ControllerPreview::Refresh);
}

ControllerPreview::~ControllerPreview() {
	Stop();
}

void ControllerPreview::SetColor(const QString& color) {
	const QColor parsed(color);
	if (m_color == parsed && m_timer.isActive()) {
		return;
	}
	m_color = parsed;
	UpdateRunning();
	for (auto* gamepad: m_gamepads) {
		ApplyColor(gamepad);
	}
}

void ControllerPreview::SetPreviewEnabled(bool enabled) {
	m_preview_enabled = enabled;
	m_timer.setInterval(enabled ? 16 : 100);
	UpdateRunning();
}

void ControllerPreview::Stop() {
	m_preview_enabled = false;
	m_color           = {};
	UpdateRunning();
}

void ControllerPreview::SetSuspended(bool suspended) {
	if (m_suspended != suspended) {
		m_suspended = suspended;
		UpdateRunning();
	}
}

void ControllerPreview::UpdateRunning() {
	if (!m_suspended && (m_preview_enabled || m_color.isValid())) {
		if (m_timer.isActive()) {
			return;
		}
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
			m_error = QString::fromUtf8(SDL_GetError());
			qWarning() << "Cannot initialize controller preview:" << m_error;
			emit StateChanged();
			return;
		}
		m_error.clear();
		int   count = 0;
		auto* ids   = SDL_GetGamepads(&count);
		for (int index = 0; index < count; ++index) {
			OpenGamepad(ids[index]);
		}
		SDL_free(ids);
		m_timer.start();
		Refresh();
		emit StateChanged();
		return;
	}
	if (m_timer.isActive()) {
		m_timer.stop();
		for (auto* gamepad: m_gamepads) {
			SDL_CloseGamepad(gamepad);
		}
		m_gamepads.clear();
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
	}
	m_pressed.clear();
	m_left_stick  = {};
	m_right_stick = {};
	m_device_name.clear();
	m_error = m_suspended ? tr("Preview is paused while a game is running.") : QString();
	emit StateChanged();
}

void ControllerPreview::OpenGamepad(SDL_JoystickID id) {
	if (!m_gamepads.contains(id)) {
		if (auto* gamepad = SDL_OpenGamepad(id); gamepad != nullptr) {
			m_gamepads.insert(id, gamepad);
			ApplyColor(gamepad);
		}
	}
}

void ControllerPreview::Refresh() {
	// One SDL event pump serves both the live diagram and lightbar preview.
	// It also delivers deferred Bluetooth DualSense LED commands.
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
			OpenGamepad(event.gdevice.which);
		} else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
			if (auto* gamepad = m_gamepads.take(event.gdevice.which); gamepad != nullptr) {
				SDL_CloseGamepad(gamepad);
			}
		}
	}

	QSet<QString> pressed;
	QPointF       left;
	QPointF       right;
	QString       name;
	if (!m_gamepads.isEmpty()) {
		auto* gamepad = m_gamepads.first();
		name          = QString::fromUtf8(SDL_GetGamepadName(gamepad));
		for (const auto& binding: BUTTONS) {
			if (SDL_GetGamepadButton(gamepad, binding.button)) {
				pressed.insert(QString::fromLatin1(binding.control));
			}
		}
		const auto stick = [&](SDL_GamepadAxis x, SDL_GamepadAxis y, const char* prefix) {
			QPointF position(Axis(gamepad, x), Axis(gamepad, y));
			if (std::hypot(position.x(), position.y()) < 0.15) {
				position = {};
			}
			const auto id = QString::fromLatin1(prefix);
			if (position.x() < -0.25) pressed.insert(id + QStringLiteral("Left"));
			if (position.x() > 0.25) pressed.insert(id + QStringLiteral("Right"));
			if (position.y() < -0.25) pressed.insert(id + QStringLiteral("Up"));
			if (position.y() > 0.25) pressed.insert(id + QStringLiteral("Down"));
			return position;
		};
		left  = stick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, "LeftStick");
		right = stick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, "RightStick");
		if (Axis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.05) {
			pressed.insert(QStringLiteral("L2"));
		}
		if (Axis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.05) {
			pressed.insert(QStringLiteral("R2"));
		}
		bool touching = false;
		if (SDL_GetNumGamepadTouchpads(gamepad) > 0) {
			for (int finger = 0; finger < SDL_GetNumGamepadTouchpadFingers(gamepad, 0); ++finger) {
				bool  down = false;
				float x    = 0;
				if (SDL_GetGamepadTouchpadFinger(gamepad, 0, finger, &down, &x, nullptr, nullptr) &&
				    down) {
					pressed.insert(x < 0.5f ? QStringLiteral("TouchPad")
					                        : QStringLiteral("TouchPadRight"));
					touching = true;
				}
			}
		}
		if (!touching && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD)) {
			pressed.insert(QStringLiteral("TouchPad"));
			pressed.insert(QStringLiteral("TouchPadRight"));
		}
	}
	if (pressed != m_pressed || left != m_left_stick || right != m_right_stick ||
	    name != m_device_name) {
		m_pressed     = pressed;
		m_left_stick  = left;
		m_right_stick = right;
		m_device_name = name;
		emit StateChanged();
	}
}

void ControllerPreview::ApplyColor(SDL_Gamepad* gamepad) const {
	if (m_color.isValid() &&
	    SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad),
	                           SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false) &&
	    !SDL_SetGamepadLED(gamepad, static_cast<Uint8>(m_color.red()),
	                       static_cast<Uint8>(m_color.green()),
	                       static_cast<Uint8>(m_color.blue()))) {
		qWarning() << "Cannot set controller LED:" << SDL_GetError();
	}
}
