#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_

namespace Libs::Vr {

// A pose in the tracking space: metres, right-handed, +y up, -z forward (the OpenXR convention).
struct VrPose {
	bool  valid          = false;
	float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
	float position[3]    = {0.0f, 0.0f, 0.0f};
};

// The pose b, given relative to the pose a, in the space that a is given in.
[[nodiscard]] inline VrPose Compose(const VrPose& a, const VrPose& b) {
	const auto [x, y, z, w]     = a.orientation;
	const auto [bx, by, bz, bw] = b.orientation;
	const auto [px, py, pz]     = b.position;
	// b's position rotated by a is p + w * t + a x t, with t = 2 * a x p.
	const float tx = 2.0f * (y * pz - z * py);
	const float ty = 2.0f * (z * px - x * pz);
	const float tz = 2.0f * (x * py - y * px);
	return {a.valid && b.valid,
	        {w * bx + x * bw + y * bz - z * by, w * by - x * bz + y * bw + z * bx,
	         w * bz + x * by - y * bx + z * bw, w * bw - x * bx - y * by - z * bz},
	        {a.position[0] + px + w * tx + y * tz - z * ty,
	         a.position[1] + py + w * ty + z * tx - x * tz,
	         a.position[2] + pz + w * tz + x * ty - y * tx}};
}

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

// The tracking origin in the stage space, whose floor is at y = 0; not valid without a stage.
[[nodiscard]] VrPose OpenXrGetOriginInStage();

// Latest state of the left (0) or right (1) controller. While it is not connected the caller keeps
// using the keyboard or gamepad.
[[nodiscard]] VrControllerInput OpenXrGetController(int hand);

} // namespace Libs::Vr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_
