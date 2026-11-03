#include "graphics/presentation/openxrHeadset.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/vrInput.h"
#include "libs/hmd2.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <thread>

namespace Libs::Vr {

namespace {

using namespace Libs::Graphics;

enum Action { TRIGGER, SQUEEZE, STICK, STICK_CLICK, PRIMARY, SECONDARY, MENU, GRIP, ACTION_COUNT };

// Touch controller bindings: the path under /user/hand/<side>/input/ per hand, or null.
constexpr struct {
	const char*  name;
	XrActionType type;
	const char*  path[2];
} ACTIONS[ACTION_COUNT] = {
    {"trigger", XR_ACTION_TYPE_FLOAT_INPUT, {"trigger/value", "trigger/value"}},
    {"squeeze", XR_ACTION_TYPE_FLOAT_INPUT, {"squeeze/value", "squeeze/value"}},
    {"thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, {"thumbstick", "thumbstick"}},
    {"thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, {"thumbstick/click", "thumbstick/click"}},
    {"primary", XR_ACTION_TYPE_BOOLEAN_INPUT, {"x/click", "a/click"}},
    {"secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, {"y/click", "b/click"}},
    {"menu", XR_ACTION_TYPE_BOOLEAN_INPUT, {"menu/click", nullptr}},
    {"grip_pose", XR_ACTION_TYPE_POSE_INPUT, {"grip/pose", "grip/pose"}},
};

constexpr const char* HANDS[2] = {"left", "right"};

// Eye images move from the render thread to the headset thread through these. One is being written,
// one is the latest recorded frame and one may be held by the headset thread until the GPU is done.
constexpr int STAGING_COUNT = 3;

struct Staging {
	VulkanImage image;
	uint64_t    tick = 0;
	VrPose head; // The head pose the game last read, so the picture can be placed in the world.
};

struct State {
	XrInstance  instance    = XR_NULL_HANDLE;
	XrSystemId  system      = XR_NULL_SYSTEM_ID;
	XrSession   session     = XR_NULL_HANDLE;
	XrSpace     view_space  = XR_NULL_HANDLE;
	XrSpace     local_space = XR_NULL_HANDLE;
	XrSwapchain swapchain   = XR_NULL_HANDLE; // Two array layers, one per eye.
	std::vector<XrSwapchainImageVulkanKHR> swapchain_images;
	vk::Extent2D                           eye {};
	std::vector<std::string>               extensions[2]; // Vulkan instance and device extensions.
	PFN_xrGetVulkanGraphicsDeviceKHR       get_graphics_device = nullptr;

	GraphicContext*  graphics = nullptr;
	std::thread      thread;
	std::atomic_bool running {false};

	// Headset thread only.
	XrSessionState    session_state   = XR_SESSION_STATE_UNKNOWN;
	bool              session_running = false;
	uint64_t          shown_tick      = 0;
	VrPose            shown_head;
	vk::CommandPool   pool;
	vk::CommandBuffer command;
	vk::Fence         fence;

	std::mutex       mutex;
	Staging          staging[STAGING_COUNT];
	int              latest    = -1;
	int              held      = -1;
	MasterSemaphore* semaphore = nullptr;

	XrActionSet action_set = XR_NULL_HANDLE;
	XrAction    actions[ACTION_COUNT] {};
	XrPath      hand_path[2] {};
	XrSpace     grip_space[2] {};

	std::mutex        input_mutex;
	VrControllerInput input[2];
	VrPose            head;
	VrPose            last_head_read;
};

State g;

bool Ok(XrResult result, const char* what) {
	if (XR_SUCCEEDED(result)) {
		return true;
	}
	char text[XR_MAX_RESULT_STRING_SIZE] = {};
	xrResultToString(g.instance, result, text);
	Log::WriteToConsoleAndLog(fmt::format("OpenXR: {} failed: {}\n", what, text));
	return false;
}

// Destroying the instance also destroys the session, spaces, swapchain and actions.
void DestroyInstance(const char* reason) {
	if (reason != nullptr) {
		Log::WriteToConsoleAndLog(
		    fmt::format("OpenXR: {}; showing the eye image in the window only\n", reason));
	}
	if (g.instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g.instance);
	}
	g.instance = XR_NULL_HANDLE;
	g.session  = XR_NULL_HANDLE;
}

struct Access {
	vk::PipelineStageFlags2 stage;
	vk::AccessFlags2        access;
};

constexpr Access NO_ACCESS {vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone};
constexpr Access TRANSFER {vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eNone};
constexpr Access TRANSFER_READ {vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferRead};
constexpr Access TRANSFER_WRITE {vk::PipelineStageFlagBits2::eTransfer,
                                 vk::AccessFlagBits2::eTransferWrite};
constexpr Access COLOR_ATTACHMENT {vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                   vk::AccessFlagBits2::eColorAttachmentRead |
                                       vk::AccessFlagBits2::eColorAttachmentWrite};

void Barrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             uint32_t layers, Access src, Access dst) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = src.stage;
	barrier.srcAccessMask       = src.access;
	barrier.dstStageMask        = dst.stage;
	barrier.dstAccessMask       = dst.access;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
	const XrVector3f t {2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z),
	                    2.0f * (q.x * v.y - q.y * v.x)};
	return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
	        v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

VrPose Locate(XrSpace space, XrTime time) {
	VrPose          pose;
	XrSpaceLocation location {XR_TYPE_SPACE_LOCATION};
	if (XR_SUCCEEDED(xrLocateSpace(space, g.local_space, time, &location)) &&
	    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
		const auto& [q, p] = location.pose;
		pose               = {true, {q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}};
	}
	return pose;
}

// The swapchain stores the eyes in a gamma-encoded format, so a raw copy of the display-encoded
// game image comes out right. Staging uses the same layout with UNORM, which a blit writes from the
// raw UNORM or float presentation frame without conversion.
bool CreateSwapchain() {
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(g.session, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(g.session, count, &count, formats.data());
	constexpr std::pair<vk::Format, vk::Format> candidates[] = {
	    {vk::Format::eR8G8B8A8Srgb, vk::Format::eR8G8B8A8Unorm},
	    {vk::Format::eB8G8R8A8Srgb, vk::Format::eB8G8R8A8Unorm},
	    {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm},
	    {vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Unorm},
	};
	const auto* format =
	    std::find_if(std::begin(candidates), std::end(candidates), [&](const auto& c) {
		    return std::find(formats.begin(), formats.end(), static_cast<int64_t>(c.first)) !=
			       formats.end();
	    });
	if (format == std::end(candidates)) {
		return false;
	}
	XrSwapchainCreateInfo info {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.format     = static_cast<int64_t>(format->first);
	info.sampleCount = 1;
	info.width       = g.eye.width;
	info.height      = g.eye.height;
	info.faceCount   = 1;
	info.arraySize   = 2;
	info.mipCount    = 1;
	if (!Ok(xrCreateSwapchain(g.session, &info, &g.swapchain), "xrCreateSwapchain")) {
		return false;
	}
	xrEnumerateSwapchainImages(g.swapchain, 0, &count, nullptr);
	g.swapchain_images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	xrEnumerateSwapchainImages(
	    g.swapchain, count, &count,
	    reinterpret_cast<XrSwapchainImageBaseHeader*>(g.swapchain_images.data()));

	vk::ImageCreateInfo staging {};
	staging.imageType   = vk::ImageType::e2D;
	staging.format      = format->second;
	staging.extent      = vk::Extent3D {g.eye.width * 2, g.eye.height, 1};
	staging.mipLevels   = 1;
	staging.arrayLayers = 1;
	staging.samples     = vk::SampleCountFlagBits::e1;
	staging.tiling      = vk::ImageTiling::eOptimal;
	staging.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	for (auto& slot: g.staging) {
		if (!g.graphics->CreateImage(staging, slot.image)) {
			return false;
		}
	}
	Log::WriteToConsoleAndLog(fmt::format("OpenXR: eye swapchain {}x{} {}\n", g.eye.width,
	                                      g.eye.height, vk::to_string(format->first)));
	return true;
}

bool CreateInput() {
	XrActionSetCreateInfo set {XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(set.actionSetName, "gameplay");
	std::strcpy(set.localizedActionSetName, "Gameplay");
	if (!Ok(xrCreateActionSet(g.instance, &set, &g.action_set), "xrCreateActionSet")) {
		return false;
	}
	for (int hand = 0; hand < 2; hand++) {
		xrStringToPath(g.instance, fmt::format("/user/hand/{}", HANDS[hand]).c_str(),
		               &g.hand_path[hand]);
	}
	std::vector<XrActionSuggestedBinding> bindings;
	for (int i = 0; i < ACTION_COUNT; i++) {
		XrActionCreateInfo info {XR_TYPE_ACTION_CREATE_INFO};
		info.actionType          = ACTIONS[i].type;
		info.countSubactionPaths = 2;
		info.subactionPaths      = g.hand_path;
		std::strcpy(info.actionName, ACTIONS[i].name);
		std::strcpy(info.localizedActionName, ACTIONS[i].name);
		if (!Ok(xrCreateAction(g.action_set, &info, &g.actions[i]), "xrCreateAction")) {
			return false;
		}
		for (int hand = 0; hand < 2; hand++) {
			if (ACTIONS[i].path[hand] != nullptr) {
				XrPath path = XR_NULL_PATH;
				xrStringToPath(
				    g.instance,
				    fmt::format("/user/hand/{}/input/{}", HANDS[hand], ACTIONS[i].path[hand])
				        .c_str(),
				    &path);
				bindings.push_back({g.actions[i], path});
			}
		}
	}
	XrInteractionProfileSuggestedBinding suggested {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller",
	               &suggested.interactionProfile);
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
	suggested.suggestedBindings      = bindings.data();
	XrSessionActionSetsAttachInfo attach {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets      = &g.action_set;
	if (!Ok(xrSuggestInteractionProfileBindings(g.instance, &suggested), "suggest bindings") ||
	    !Ok(xrAttachSessionActionSets(g.session, &attach), "attach action sets")) {
		return false;
	}
	for (int hand = 0; hand < 2; hand++) {
		XrActionSpaceCreateInfo space {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		space.action            = g.actions[GRIP];
		space.subactionPath     = g.hand_path[hand];
		space.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
		if (!Ok(xrCreateActionSpace(g.session, &space, &g.grip_space[hand]), "grip space")) {
			return false;
		}
	}
	return true;
}

// The head is tracked while the frame loop runs; the controllers only while the session has focus.
void PollInput(XrTime time) {
	const auto        head = Locate(g.view_space, time);
	VrControllerInput input[2];
	XrActiveActionSet active {g.action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync {XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets      = &active;
	if (g.action_set != XR_NULL_HANDLE && g.session_state == XR_SESSION_STATE_FOCUSED &&
	    XR_SUCCEEDED(xrSyncActions(g.session, &sync))) {
		for (int hand = 0; hand < 2; hand++) {
			const auto get = [&](Action action, auto read, auto state) {
				XrActionStateGetInfo info {XR_TYPE_ACTION_STATE_GET_INFO};
				info.action        = g.actions[action];
				info.subactionPath = g.hand_path[hand];
				read(g.session, &info, &state);
				return state;
			};
			const auto value = [&](Action action) {
				return get(action, xrGetActionStateFloat,
				           XrActionStateFloat {XR_TYPE_ACTION_STATE_FLOAT})
				    .currentState;
			};
			const auto flag = [&](Action action) {
				return get(action, xrGetActionStateBoolean,
				           XrActionStateBoolean {XR_TYPE_ACTION_STATE_BOOLEAN})
				           .currentState == XR_TRUE;
			};
			const auto stick = get(STICK, xrGetActionStateVector2f,
			                       XrActionStateVector2f {XR_TYPE_ACTION_STATE_VECTOR2F});
			auto&      out   = input[hand];
			out.trigger      = value(TRIGGER);
			out.squeeze      = value(SQUEEZE);
			out.stick_x      = stick.currentState.x;
			out.stick_y      = stick.currentState.y;
			out.stick_click  = flag(STICK_CLICK);
			out.primary      = flag(PRIMARY);
			out.secondary    = flag(SECONDARY);
			out.menu         = flag(MENU);
			out.pose         = Locate(g.grip_space[hand], time);
			out.connected =
			    get(GRIP, xrGetActionStatePose, XrActionStatePose {XR_TYPE_ACTION_STATE_POSE})
			        .isActive == XR_TRUE;
		}
	}
	std::lock_guard lock(g.input_mutex);
	g.head     = head;
	g.input[0] = input[0];
	g.input[1] = input[1];
}

void HandleEvents() {
	for (XrEventDataBuffer event {XR_TYPE_EVENT_DATA_BUFFER};
	     xrPollEvent(g.instance, &event) == XR_SUCCESS; event = {XR_TYPE_EVENT_DATA_BUFFER}) {
		if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g.running = false;
			continue;
		}
		if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			continue;
		}
		g.session_state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
		LOGF("OpenXR: session state %d\n", static_cast<int>(g.session_state));
		switch (g.session_state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin {XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				g.session_running = Ok(xrBeginSession(g.session, &begin), "xrBeginSession");
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				xrEndSession(g.session);
				g.session_running = false;
				std::lock_guard lock(g.input_mutex);
				g.head     = {};
				g.input[0] = {};
				g.input[1] = {};
				break;
			}
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING: g.running = false; break;
			default: break;
		}
	}
}

bool CopyToSwapchain(const VulkanImage& staging) {
	uint32_t                    image = 0;
	XrSwapchainImageAcquireInfo acquire {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	{
		Common::LockGuard queue_lock(g.graphics->queue_mutex);
		if (!Ok(xrAcquireSwapchainImage(g.swapchain, &acquire, &image), "acquire")) {
			return false;
		}
	}
	XrSwapchainImageWaitInfo wait {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	bool copied  = Ok(xrWaitSwapchainImage(g.swapchain, &wait), "wait swapchain image");
	if (copied) {
		const auto target = g.swapchain_images[image].image;
		(void)g.command.reset();
		vk::CommandBufferBeginInfo begin {};
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		(void)g.command.begin(&begin);
		Barrier(g.command, target, vk::ImageLayout::eUndefined,
		        vk::ImageLayout::eTransferDstOptimal, 2, NO_ACCESS, TRANSFER_WRITE);
		vk::ImageCopy copies[2] {};
		for (uint32_t eye = 0; eye < 2; eye++) {
			copies[eye].srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			copies[eye].srcOffset = vk::Offset3D {static_cast<int32_t>(eye * g.eye.width), 0, 0};
			copies[eye].dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, eye, 1};
			copies[eye].extent         = vk::Extent3D {g.eye.width, g.eye.height, 1};
		}
		g.command.copyImage(staging.image, vk::ImageLayout::eTransferSrcOptimal, target,
		                    vk::ImageLayout::eTransferDstOptimal, 2, copies);
		Barrier(g.command, target, vk::ImageLayout::eTransferDstOptimal,
		        vk::ImageLayout::eColorAttachmentOptimal, 2, TRANSFER_WRITE, COLOR_ATTACHMENT);
		(void)g.command.end();
		vk::SubmitInfo submit {};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &g.command;
		(void)g.graphics->device.resetFences(1, &g.fence);
		vk::Result result {};
		{
			Common::LockGuard queue_lock(g.graphics->queue_mutex);
			result = g.graphics->queue.submit(1, &submit, g.fence);
		}
		copied = result == vk::Result::eSuccess &&
		         g.graphics->device.waitForFences(1, &g.fence, VK_TRUE, UINT64_MAX) ==
		             vk::Result::eSuccess;
	}
	XrSwapchainImageReleaseInfo release {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	Common::LockGuard           queue_lock(g.graphics->queue_mutex);
	return Ok(xrReleaseSwapchainImage(g.swapchain, &release), "release") && copied;
}

// Holds the newest recorded eye image until the GPU has finished it, without waiting, then copies
// it into the swapchain. The swapchain keeps the last copy, so frames in between show it again.
bool UpdateSwapchain() {
	int              index     = -1;
	MasterSemaphore* semaphore = nullptr;
	{
		std::lock_guard lock(g.mutex);
		if (g.held < 0 && g.latest >= 0 && g.staging[g.latest].tick != g.shown_tick) {
			g.held = g.latest;
		}
		index     = g.held;
		semaphore = g.semaphore;
	}
	if (index >= 0) {
		const auto& slot = g.staging[index];
		semaphore->Refresh();
		if (semaphore->IsFree(slot.tick)) {
			if (CopyToSwapchain(slot.image)) {
				g.shown_tick = slot.tick;
				g.shown_head = slot.head;
			}
			std::lock_guard lock(g.mutex);
			g.held = -1;
		}
	}
	return g.shown_tick != 0;
}

// The game drew the picture for the head pose it read, so it is placed in the world at that pose
// and the runtime reprojects it as the head moves. Eye offsets come from the runtime.
void PlaceViews(XrTime time, XrCompositionLayerProjection& layer,
                XrCompositionLayerProjectionView (&views)[2]) {
	XrViewLocateInfo locate {XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime           = time;
	locate.space                 = g.view_space;
	XrViewState state {XR_TYPE_VIEW_STATE};
	XrView      located[2] {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t    count = 0;
	const bool  valid =
	    XR_SUCCEEDED(xrLocateViews(g.session, &locate, &state, 2, &count, located)) &&
	    (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
	const auto&         head = g.shown_head;
	const XrQuaternionf head_q {head.orientation[0], head.orientation[1], head.orientation[2],
	                            head.orientation[3]};
	const XrVector3f    head_p {head.position[0], head.position[1], head.position[2]};
	// The outer side of each eye is its temporal side: left for the left eye.
	const float out    = std::atan(Hmd2::FOV_TAN_OUT);
	const float in     = std::atan(Hmd2::FOV_TAN_IN);
	const float top    = std::atan(Hmd2::FOV_TAN_TOP);
	const float bottom = std::atan(Hmd2::FOV_TAN_BOTTOM);
	for (int eye = 0; eye < 2; eye++) {
		const float side = eye == 0 ? -Hmd2::EYE_OFFSET_M : Hmd2::EYE_OFFSET_M;
		auto&       view = views[eye];
		view.pose        = valid ? located[eye].pose : XrPosef {{0, 0, 0, 1}, {side, 0, 0}};
		if (head.valid) {
			const auto offset     = Rotate(head_q, view.pose.position);
			view.pose.position    = {head_p.x + offset.x, head_p.y + offset.y, head_p.z + offset.z};
			view.pose.orientation = Multiply(head_q, view.pose.orientation);
		}
		view.fov                = {eye == 0 ? -out : -in, eye == 0 ? in : out, top, -bottom};
		view.subImage.swapchain = g.swapchain;
		view.subImage.imageRect = {
		    {0, 0}, {static_cast<int32_t>(g.eye.width), static_cast<int32_t>(g.eye.height)}};
		view.subImage.imageArrayIndex = static_cast<uint32_t>(eye);
	}
	layer.space     = head.valid ? g.local_space : g.view_space;
	layer.viewCount = 2;
	layer.views     = views;
}

void ThreadMain() {
	while (g.running) {
		HandleEvents();
		XrFrameWaitInfo wait {XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState    frame {XR_TYPE_FRAME_STATE};
		if (!g.session_running || !Ok(xrWaitFrame(g.session, &wait, &frame), "xrWaitFrame")) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		PollInput(frame.predictedDisplayTime);
		XrFrameBeginInfo begin {XR_TYPE_FRAME_BEGIN_INFO};
		{
			Common::LockGuard queue_lock(g.graphics->queue_mutex);
			if (!Ok(xrBeginFrame(g.session, &begin), "xrBeginFrame")) {
				continue;
			}
		}
		XrCompositionLayerProjection     layer {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		XrCompositionLayerProjectionView views[2] {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
		                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		const auto*    layers = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
		XrFrameEndInfo end {XR_TYPE_FRAME_END_INFO};
		end.displayTime          = frame.predictedDisplayTime;
		end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (frame.shouldRender == XR_TRUE && UpdateSwapchain()) {
			PlaceViews(frame.predictedDisplayTime, layer, views);
			end.layerCount = 1;
			end.layers     = &layers;
		}
		Common::LockGuard queue_lock(g.graphics->queue_mutex);
		(void)Ok(xrEndFrame(g.session, &end), "xrEndFrame");
	}
}

} // namespace

void OpenXrInit() {
	if (!Config::VrEnabled()) {
		return;
	}
	if (const char* env = std::getenv("KYTY_OPENXR");
	    env != nullptr && std::strcmp(env, "0") == 0) {
		return;
	}
	const char*          extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo info {XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(info.applicationInfo.applicationName, "KytyPS5");
	std::strcpy(info.applicationInfo.engineName, "Kyty");
	info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	info.enabledExtensionCount      = 1;
	info.enabledExtensionNames      = extensions;
	if (XR_FAILED(xrCreateInstance(&info, &g.instance))) {
		DestroyInstance("no runtime available");
		return;
	}
	XrSystemGetInfo system {XR_TYPE_SYSTEM_GET_INFO};
	system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(xrGetSystem(g.instance, &system, &g.system))) {
		DestroyInstance("no headset available");
		return;
	}
	PFN_xrGetVulkanGraphicsRequirementsKHR get_requirements = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR   get_extensions[2] {};
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsRequirementsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanInstanceExtensionsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&get_extensions[0]));
	// Both queries share one signature.
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanDeviceExtensionsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&get_extensions[1]));
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsDeviceKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&g.get_graphics_device));
	XrGraphicsRequirementsVulkanKHR requirements {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	if (get_requirements == nullptr || get_extensions[0] == nullptr ||
	    get_extensions[1] == nullptr || g.get_graphics_device == nullptr ||
	    XR_FAILED(get_requirements(g.instance, g.system, &requirements))) {
		DestroyInstance("the runtime does not support XR_KHR_vulkan_enable");
		return;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t size = 0;
		get_extensions[i](g.instance, g.system, 0, &size, nullptr);
		std::string names(size, '\0');
		get_extensions[i](g.instance, g.system, size, &size, names.data());
		g.extensions[i] = Common::Split(names.c_str(), ' ');
	}
	XrViewConfigurationView views[2] {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
	                                  {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	uint32_t                count = 0;
	xrEnumerateViewConfigurationViews(g.instance, g.system,
	                                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views);
	g.eye = {views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight};
	XrSystemProperties properties {XR_TYPE_SYSTEM_PROPERTIES};
	xrGetSystemProperties(g.instance, g.system, &properties);
	Log::WriteToConsoleAndLog(fmt::format("OpenXR: headset '{}' ready\n", properties.systemName));
}

void OpenXrAppendExtensions(std::vector<const char*>&                   out,
                            const std::vector<vk::ExtensionProperties>& available, bool device) {
	if (g.instance == XR_NULL_HANDLE) {
		return;
	}
	for (const auto& name: g.extensions[device ? 1 : 0]) {
		const auto same = [&](const char* other) { return name == other; };
		if (std::any_of(available.begin(), available.end(),
		                [&](const auto& extension) { return same(extension.extensionName); }) &&
		    std::none_of(out.begin(), out.end(), same)) {
			out.push_back(name.c_str());
		}
	}
}

bool OpenXrPhysicalDevice(vk::Instance instance, vk::PhysicalDevice& out) {
	VkPhysicalDevice device = VK_NULL_HANDLE;
	if (g.instance == XR_NULL_HANDLE ||
	    XR_FAILED(g.get_graphics_device(g.instance, g.system, static_cast<VkInstance>(instance),
	                                    &device))) {
		return false;
	}
	out = vk::PhysicalDevice(device);
	return true;
}

void OpenXrStartSession(GraphicContext& graphics) {
	if (g.instance == XR_NULL_HANDLE) {
		return;
	}
	// The runtime must be asked for its GPU before a session is created.
	vk::PhysicalDevice required;
	if (!OpenXrPhysicalDevice(graphics.instance, required) ||
	    required != graphics.physical_device) {
		DestroyInstance("the headset runs on another GPU; select it with --gpu-index");
		return;
	}
	g.graphics = &graphics;
	XrGraphicsBindingVulkanKHR binding {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance         = static_cast<VkInstance>(graphics.instance);
	binding.physicalDevice   = static_cast<VkPhysicalDevice>(graphics.physical_device);
	binding.device           = static_cast<VkDevice>(graphics.device);
	binding.queueFamilyIndex = graphics.queue_family;
	binding.queueIndex       = 0;
	XrSessionCreateInfo info {XR_TYPE_SESSION_CREATE_INFO};
	info.next     = &binding;
	info.systemId = g.system;
	XrReferenceSpaceCreateInfo space {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
	const auto reference       = [&](XrReferenceSpaceType type, XrSpace& out) {
		space.referenceSpaceType = type;
		return Ok(xrCreateReferenceSpace(g.session, &space, &out), "xrCreateReferenceSpace");
	};
	if (!Ok(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession") ||
	    !reference(XR_REFERENCE_SPACE_TYPE_VIEW, g.view_space) ||
	    !reference(XR_REFERENCE_SPACE_TYPE_LOCAL, g.local_space) || !CreateSwapchain()) {
		DestroyInstance("the headset session could not be created");
		return;
	}
	if (!CreateInput()) {
		g.action_set = XR_NULL_HANDLE;
	}
	vk::CommandPoolCreateInfo pool {};
	pool.queueFamilyIndex = graphics.queue_family;
	pool.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	(void)graphics.device.createCommandPool(&pool, nullptr, &g.pool);
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = g.pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	(void)graphics.device.allocateCommandBuffers(&allocate, &g.command);
	vk::FenceCreateInfo fence {};
	(void)graphics.device.createFence(&fence, nullptr, &g.fence);

	g.running = true;
	g.thread  = std::thread(ThreadMain);
}

void OpenXrShutdown() {
	g.running = false;
	if (g.thread.joinable()) {
		g.thread.join();
	}
	DestroyInstance(nullptr);
	if (g.graphics == nullptr) {
		return;
	}
	{
		Common::LockGuard queue_lock(g.graphics->queue_mutex);
		(void)g.graphics->device.waitIdle();
	}
	for (auto& slot: g.staging) {
		if (slot.image.image != nullptr) {
			g.graphics->DeleteImage(slot.image);
		}
	}
	g.graphics->device.destroyFence(g.fence, nullptr);
	g.graphics->device.destroyCommandPool(g.pool, nullptr);
	g.graphics = nullptr;
}

VrPose OpenXrGetHeadPose() {
	if (!g.running) {
		return {};
	}
	std::lock_guard lock(g.input_mutex);
	if (g.head.valid) {
		g.last_head_read = g.head;
	}
	return g.head;
}

VrControllerInput OpenXrGetController(int hand) {
	if (!g.running) {
		return {};
	}
	std::lock_guard lock(g.input_mutex);
	return g.input[hand];
}

void OpenXrQueueEyeSource(CommandBuffer& command, const VulkanImage& source) {
	if (!g.running) {
		return;
	}
	std::lock_guard lock(g.mutex);
	int             index = 0;
	while (index == g.held || index == g.latest) {
		index++;
	}
	auto&      slot       = g.staging[index];
	const auto vk_command = command.Handle();
	Barrier(vk_command, slot.image.image, vk::ImageLayout::eUndefined,
	        vk::ImageLayout::eTransferDstOptimal, 1, TRANSFER, TRANSFER_WRITE);
	vk::ImageBlit blit {};
	blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(source.extent.width),
	                                    static_cast<int32_t>(source.extent.height), 1};
	blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(slot.image.extent.width),
	                                    static_cast<int32_t>(slot.image.extent.height), 1};
	vk_command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal, slot.image.image,
	                     vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);
	Barrier(vk_command, slot.image.image, vk::ImageLayout::eTransferDstOptimal,
	        vk::ImageLayout::eTransferSrcOptimal, 1, TRANSFER_WRITE, TRANSFER_READ);
	auto& scheduler = command.GetContext().GetCommandScheduler();
	slot.tick       = scheduler.CurrentTick();
	g.semaphore     = &scheduler.GetMasterSemaphore();
	g.latest        = index;
	std::lock_guard input_lock(g.input_mutex);
	slot.head = g.last_head_read;
}

} // namespace Libs::Vr
