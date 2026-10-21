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

// Host-side names; they must match PAD_SOURCES in hostInput.cpp.
constexpr struct {
	SDL_GamepadButton button;
	const char*       name;
} RAW_BUTTONS[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, "South"},
    {SDL_GAMEPAD_BUTTON_EAST, "East"},
    {SDL_GAMEPAD_BUTTON_WEST, "West"},
    {SDL_GAMEPAD_BUTTON_NORTH, "North"},
    {SDL_GAMEPAD_BUTTON_BACK, "Back"},
    {SDL_GAMEPAD_BUTTON_GUIDE, "Guide"},
    {SDL_GAMEPAD_BUTTON_START, "Start"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "LeftStick"},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "RightStick"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LeftShoulder"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RightShoulder"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, "DpadUp"},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "DpadDown"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "DpadLeft"},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "DpadRight"},
    {SDL_GAMEPAD_BUTTON_MISC1, "Misc1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, "RightPaddle1"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, "LeftPaddle1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, "RightPaddle2"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, "LeftPaddle2"},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, "Touchpad"},
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

void ControllerPreview::SetBatteryMonitor(bool enabled) {
	if (m_battery_enabled != enabled) {
		m_battery_enabled = enabled;
		UpdateRunning();
	}
}

void ControllerPreview::UpdateRunning() {
	if (!m_suspended && (m_preview_enabled || m_color.isValid() || m_battery_enabled)) {
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
	m_stick_dir = -1;
	m_stick_x   = 0;
	m_stick_y   = 0;
	m_raw_pressed.clear();
	m_left_stick  = {};
	m_right_stick = {};
	m_device_name.clear();
	m_battery_percent  = -1;
	m_battery_charging = false;
	m_connected        = false;
	m_wired            = false;
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
			m_stick_dir = -1;
			m_stick_x   = 0;
			m_stick_y   = 0;
		} else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
			switch (event.gbutton.button) {
				case SDL_GAMEPAD_BUTTON_DPAD_LEFT: emit NavigationInput(0); break;
				case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: emit NavigationInput(1); break;
				case SDL_GAMEPAD_BUTTON_DPAD_UP: emit NavigationInput(2); break;
				case SDL_GAMEPAD_BUTTON_DPAD_DOWN: emit NavigationInput(3); break;
				case SDL_GAMEPAD_BUTTON_SOUTH: emit NavigationInput(4); break;
				case SDL_GAMEPAD_BUTTON_EAST: emit NavigationInput(5); break;
				default: break;
			}
		} else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
		           (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ||
		            event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
			// The left stick navigates once per push, like a d-pad. The stick is tracked as a
			// whole: a diagonal push emits one action and re-arms only after returning to center.
			if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
				m_stick_x = event.gaxis.value;
			} else {
				m_stick_y = event.gaxis.value;
			}
			const int ax = std::abs(m_stick_x);
			const int ay = std::abs(m_stick_y);
			if (m_stick_dir < 0) {
				if (ax > 20000 || ay > 20000) {
					m_stick_dir = ax >= ay ? (m_stick_x < 0 ? 0 : 1) : (m_stick_y < 0 ? 2 : 3);
					emit NavigationInput(m_stick_dir);
				}
			} else if (ax < 10000 && ay < 10000) {
				m_stick_dir = -1;
			}
		}
	}

	QSet<QString> pressed;
	QSet<QString> raw;
	QPointF       left;
	QPointF       right;
	QString       name;
	int           battery  = -1;
	bool          charging = false;
	bool          connected = !m_gamepads.isEmpty();
	bool          wired     = false;
	if (!m_gamepads.isEmpty()) {
		auto* gamepad = m_gamepads.first();
		name          = QString::fromUtf8(SDL_GetGamepadName(gamepad));
		wired = SDL_GetGamepadConnectionState(gamepad) == SDL_JOYSTICK_CONNECTION_WIRED;
		// Use the first connected controller that reports a battery; the wired state describes it.
		for (auto* candidate: m_gamepads) {
			int        percent = -1;
			const auto power   = SDL_GetGamepadPowerInfo(candidate, &percent);
			if (percent >= 0 && power != SDL_POWERSTATE_NO_BATTERY && power != SDL_POWERSTATE_ERROR) {
				battery  = percent;
				// Only an actively charging pack counts; a full wired pad is just powered by USB.
				charging = power == SDL_POWERSTATE_CHARGING;
				wired    = SDL_GetGamepadConnectionState(candidate) == SDL_JOYSTICK_CONNECTION_WIRED;
				break;
			}
		}
		for (const auto& binding: RAW_BUTTONS) {
			if (SDL_GamepadHasButton(gamepad, binding.button) &&
			    SDL_GetGamepadButton(gamepad, binding.button)) {
				raw.insert(QString::fromLatin1(binding.name));
			}
		}
		if (Axis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5) {
			raw.insert(QStringLiteral("LeftTrigger"));
		}
		if (Axis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5) {
			raw.insert(QStringLiteral("RightTrigger"));
		}
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
	if (pressed != m_pressed || raw != m_raw_pressed || left != m_left_stick ||
	    right != m_right_stick || name != m_device_name || battery != m_battery_percent ||
	    charging != m_battery_charging || connected != m_connected || wired != m_wired) {
		m_connected        = connected;
		m_wired            = wired;
		m_battery_percent  = battery;
		m_battery_charging = charging;
		m_pressed     = pressed;
		m_raw_pressed = raw;
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
