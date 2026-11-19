use gilrs::{Axis, Button, EventType, Gilrs};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NavEvent {
    Left,
    Right,
    Up,
    Down,
    Confirm,
    Back,
    TabPrev,
    TabNext,
    ControlCenter,
}

pub struct GamepadManager {
    gilrs: Option<Gilrs>,
    last_stick_move: Instant,
    connected_name: Option<String>,
}

impl GamepadManager {
    pub fn new() -> Self {
        let gilrs = Gilrs::new().ok();
        let connected_name = gilrs.as_ref().and_then(|g| {
            g.gamepads()
                .next()
                .map(|(_, gp)| gp.name().to_string())
        });
        Self {
            gilrs,
            last_stick_move: Instant::now() - Duration::from_secs(1),
            connected_name,
        }
    }

    pub fn connected_controller(&self) -> Option<&str> {
        self.connected_name.as_deref()
    }

    pub fn poll(&mut self) -> Vec<NavEvent> {
        let mut events = Vec::new();
        let Some(gilrs) = self.gilrs.as_mut() else {
            return events;
        };

        while let Some(ev) = gilrs.next_event() {
            match ev.event {
                EventType::Connected => {
                    let gp = gilrs.gamepad(ev.id);
                    self.connected_name = Some(gp.name().to_string());
                }
                EventType::Disconnected => {
                    self.connected_name = gilrs
                        .gamepads()
                        .next()
                        .map(|(_, gp)| gp.name().to_string());
                }
                EventType::ButtonPressed(btn, _) => match btn {
                    Button::DPadLeft => events.push(NavEvent::Left),
                    Button::DPadRight => events.push(NavEvent::Right),
                    Button::DPadUp => events.push(NavEvent::Up),
                    Button::DPadDown => events.push(NavEvent::Down),
                    Button::South => events.push(NavEvent::Confirm),
                    Button::East => events.push(NavEvent::Back),
                    Button::LeftTrigger => events.push(NavEvent::TabPrev),
                    Button::RightTrigger => events.push(NavEvent::TabNext),
                    Button::Mode | Button::Start => events.push(NavEvent::ControlCenter),
                    _ => {}
                },
                EventType::AxisChanged(axis, val, _) => {
                    let now = Instant::now();
                    if now.duration_since(self.last_stick_move) > Duration::from_millis(210) {
                        match axis {
                            Axis::LeftStickX if val < -0.65 => {
                                events.push(NavEvent::Left);
                                self.last_stick_move = now;
                            }
                            Axis::LeftStickX if val > 0.65 => {
                                events.push(NavEvent::Right);
                                self.last_stick_move = now;
                            }
                            Axis::LeftStickY if val > 0.65 => {
                                events.push(NavEvent::Up);
                                self.last_stick_move = now;
                            }
                            Axis::LeftStickY if val < -0.65 => {
                                events.push(NavEvent::Down);
                                self.last_stick_move = now;
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
        }

        events
    }
}
