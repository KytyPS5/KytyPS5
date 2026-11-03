#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_

namespace Libs::Vr {

// A pose in the tracking space: metres, right-handed, +y up, -z forward (the OpenXR convention).
struct VrPose {
	bool  valid          = false;
	float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
	float position[3]    = {0.0f, 0.0f, 0.0f};
};

// One headset controller. Sticks are -1..1 with +y up, the trigger and squeeze 0..1.
struct VrControllerInput {
	bool   connected = false;
	VrPose pose;
	float  trigger     = 0.0f;
	float  squeeze     = 0.0f;
	float  stick_x     = 0.0f;
	float  stick_y     = 0.0f;
	bool   primary     = false; // X on the left controller, A on the right one.
	bool   secondary   = false; // Y on the left controller, B on the right one.
	bool   stick_click = false;
	bool   menu        = false;
};

// Latest head pose; not valid, at the origin, while no headset session tracks the head.
[[nodiscard]] VrPose OpenXrGetHeadPose();

// Latest state of the left (0) or right (1) controller. While it is not connected the caller keeps
// using the keyboard or gamepad.
[[nodiscard]] VrControllerInput OpenXrGetController(int hand);

} // namespace Libs::Vr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_
