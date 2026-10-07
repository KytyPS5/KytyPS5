#include "libs/dualSenseHaptics.h"

#include "common/logging/log.h"
#include "common/threads.h"
#include "libs/dualSenseBluetooth.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace Libs::Controller::DualSenseHaptics {

struct Stream {
	uint32_t                    freq       = 0;
	bool                        speaker    = false;
	int                         controller = -1;
	SDL_AudioStream*            sdl        = nullptr;
	DualSenseBluetooth::Stream* bluetooth  = nullptr;
	SDL_AudioDeviceID           device     = 0;
	uint64_t                    next_check = 0;
	std::vector<float>          buffer;
};

namespace {

constexpr int      FRAME_BYTES  = 4 * sizeof(float);
constexpr uint32_t MAX_QUEUE_MS = 80;
Common::Mutex      g_mutex;
int                g_controller      = -1;
int                g_haptics_streams = 0;
uint8_t            g_large_motor = 0, g_small_motor = 0;
uint64_t           g_rumble_until = 0, g_haptics_until = 0;
int                g_speaker_controller = -1;
std::map<int, int> g_speaker_streams;

void ApplyRumble(bool force = false) { // Caller holds g_mutex; no calls into Audio or Controller.
	SDL_LockJoysticks();
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(g_controller));
	    pad != nullptr) {
		const auto now   = SDL_GetTicks();
		const bool muted = g_haptics_until > now || g_rumble_until <= now;
		if (force && !muted) {
			// SDL caches motor strengths. Force a report after raw speaker routing changed the
			// controller's motor mode without changing that cache.
			(void)SDL_RumbleGamepad(pad, 0, 0, 0);
		}
		(void)SDL_RumbleGamepad(
		    pad, muted ? 0 : g_large_motor * 0x101U, muted ? 0 : g_small_motor * 0x101U,
		    g_rumble_until > now ? static_cast<uint32_t>(g_rumble_until - now) : 0);
	}
	SDL_UnlockJoysticks();
}

void SelectController(int controller) {
	if (g_controller != controller) {
		g_haptics_until = g_rumble_until = 0;
		ApplyRumble();
		g_controller  = controller;
		g_large_motor = g_small_motor = 0;
	}
}

bool RouteSpeaker(int controller, bool speaker) { // Caller holds g_mutex.
	// Route the right channel to the speaker and mute the headphones with the values Linux uses
	// without headphones (output report 0x02). Plugged-in headphones are not checked.
	std::array<uint8_t, 38> report {};
	report[0] = 0x80; // Audio control valid
	if (speaker) {
		report[0] |= 0x20; // Speaker volume valid
		report[1]  = 0x80; // Audio control 2 valid
		report[5]  = 0x64; // Speaker volume
		report[7]  = 0x30; // Output path: right channel to the speaker, headphones muted
		report[37] = 0x02; // Speaker preamp +6 dB
	}
	bool routed = false;
	SDL_LockJoysticks();
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(controller)); pad != nullptr) {
		routed = SDL_SendGamepadEffect(pad, report.data(), static_cast<int>(report.size()));
	}
	SDL_UnlockJoysticks();
	if (routed && controller == g_controller) {
		ApplyRumble(true);
	}
	return routed;
}

bool SelectSpeaker(int controller) { // Caller holds g_mutex; -1 restores headphone routing.
	if (g_speaker_controller == controller) {
		return true;
	}
	if (g_speaker_controller != -1) {
		RouteSpeaker(g_speaker_controller, false);
	}
	g_speaker_controller = -1;
	if (controller != -1 && !RouteSpeaker(controller, true)) {
		return false;
	}
	g_speaker_controller = controller;
	return true;
}

void SDLCALL UpdateRumble(void*, SDL_AudioStream*, int, int) {
	Common::LockGuard lock(g_mutex);
	if (g_haptics_until != 0 && SDL_GetTicks() >= g_haptics_until) {
		g_haptics_until = 0;
		ApplyRumble();
	}
}

void UpdateBluetoothRumble() {
	UpdateRumble(nullptr, nullptr, 0, 0);
}

void CloseDevice(Stream* stream) {
	const bool had_device = stream->sdl != nullptr || stream->bluetooth != nullptr;
	if (stream->bluetooth != nullptr) {
		DualSenseBluetooth::Close(stream->bluetooth);
		stream->bluetooth = nullptr;
	}
	if (stream->sdl != nullptr) {
		// Destroy outside g_mutex: SDL waits for any running get callback.
		SDL_DestroyAudioStream(stream->sdl);
		stream->sdl = nullptr;
	}
	if (had_device) {
		Common::LockGuard lock(g_mutex);
		if (!stream->speaker && --g_haptics_streams == 0 && g_haptics_until != 0) {
			g_haptics_until = 0;
			ApplyRumble();
		}
		// Route back to the headphones once no speaker port plays on the controller.
		if (stream->speaker && --g_speaker_streams.at(stream->controller) == 0) {
			g_speaker_streams.erase(stream->controller);
			if (g_speaker_controller == stream->controller) {
				SelectSpeaker(-1);
			}
		}
	}
}

bool IsWireless(int controller) {
	SDL_LockJoysticks();
	auto*      pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(controller));
	const bool wireless =
	    pad != nullptr && SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRELESS;
	SDL_UnlockJoysticks();
	return wireless;
}

bool CanUseDevice(int controller) {
	// SDL exposes no portable association between a gamepad and its USB audio endpoint.
	// Require one wired DualSense; guessing with multiple pads could play on the wrong one.
	SDL_LockJoysticks();
	int   count                 = 0;
	auto* pads                  = SDL_GetGamepads(&count);
	int   wired_dualsense_count = 0;
	bool  found                 = false;
	for (int i = 0; i < count; i++) {
		auto* candidate = SDL_GetGamepadFromID(pads[i]);
		if (candidate != nullptr && SDL_GetGamepadTypeForID(pads[i]) == SDL_GAMEPAD_TYPE_PS5 &&
		    SDL_GetGamepadConnectionState(candidate) == SDL_JOYSTICK_CONNECTION_WIRED) {
			wired_dualsense_count++;
			found |= pads[i] == static_cast<SDL_JoystickID>(controller);
		}
	}
	SDL_free(pads);
	SDL_UnlockJoysticks();
	return found && wired_dualsense_count == 1;
}

SDL_AudioDeviceID FindDevice() {
	int               count   = 0;
	auto*             devices = SDL_GetAudioPlaybackDevices(&count);
	SDL_AudioDeviceID device  = 0;
	for (int i = 0; i < count; i++) {
		const char*   name = SDL_GetAudioDeviceName(devices[i]);
		SDL_AudioSpec spec {};
		const bool is_dualsense_name =
		    name != nullptr &&
		    (SDL_strcasestr(name, "DualSense") != nullptr ||
		     SDL_strcasestr(name, "Wireless Controller") != nullptr);
		if (is_dualsense_name &&
		    SDL_GetAudioDeviceFormat(devices[i], &spec, nullptr) && spec.channels == 4) {
			if (device != 0) {
				device = 0; // Multiple matching endpoints cannot be associated reliably either.
				break;
			}
			device = devices[i];
		}
	}
	SDL_free(devices);
	return device;
}

void RefreshDevice(Stream* stream, int controller, bool wireless) {
	if (stream->controller != controller || (wireless && stream->sdl != nullptr) ||
	    (!wireless && stream->bluetooth != nullptr)) {
		CloseDevice(stream);
		stream->controller = controller;
		stream->next_check = 0;
	}
	const auto now = SDL_GetTicks();
	if (stream->bluetooth != nullptr || now < stream->next_check) {
		return;
	}
	stream->next_check = now + 2000;
	const auto device  = wireless ? 0 : FindDevice();
	if (stream->device == device && stream->sdl != nullptr) {
		return;
	}
	CloseDevice(stream);
	stream->device = device;
	if (!wireless && device == 0) {
		return;
	}
	SDL_AudioStream*            sdl       = nullptr;
	DualSenseBluetooth::Stream* bluetooth = nullptr;
	bool                        ready;
	if (wireless) {
		bluetooth = DualSenseBluetooth::Open(stream->freq, stream->speaker, controller,
		                                     UpdateBluetoothRumble);
		ready     = bluetooth != nullptr;
	} else {
		const SDL_AudioSpec desired {SDL_AUDIO_F32, 4, static_cast<int>(stream->freq)};
		SDL_AudioSpec       actual {};
		sdl   = SDL_OpenAudioDeviceStream(device, &desired, UpdateRumble, nullptr);
		ready = sdl != nullptr &&
		        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sdl), &actual, nullptr) &&
		        actual.channels == 4 && SDL_ResumeAudioStreamDevice(sdl);
	}
	if (ready) {
		Common::LockGuard lock(g_mutex);
		if (stream->speaker) {
			ready = SelectSpeaker(controller);
			if (ready) {
				g_speaker_streams[controller]++;
			}
		} else {
			g_haptics_streams++;
		}
	}
	if (ready) {
		stream->sdl       = sdl;
		stream->bluetooth = bluetooth;
		if (!wireless) {
			LOGF("DualSenseHaptics: playing on '%s'\n", SDL_GetAudioDeviceName(device));
		}
	} else {
		DualSenseBluetooth::Close(bluetooth);
		SDL_DestroyAudioStream(sdl);
		if (!wireless) {
			LOGF("DualSenseHaptics: cannot open quad output: %s\n", SDL_GetError());
		}
	}
}

uint64_t QueueUsbAudio(Stream* stream, uint32_t frames) {
	auto      queued = std::max(0, SDL_GetAudioStreamQueued(stream->sdl));
	const int bytes  = static_cast<int>(frames * FRAME_BYTES);
	const int max_bytes =
	    static_cast<int>(static_cast<uint64_t>(stream->freq) * FRAME_BYTES * MAX_QUEUE_MS / 1000);
	if (queued + bytes > max_bytes) {
		// Keep the newest sound after a brief device stall. Clearing the entire
		// stream produces a gap that is much longer than one audio block.
		SDL_AudioSpec              target {};
		std::array<uint8_t, 16384> discarded {};
		if (SDL_GetAudioStreamFormat(stream->sdl, nullptr, &target)) {
			const int frame_bytes = SDL_AUDIO_FRAMESIZE(target);
			if (frame_bytes > 0) {
				const int chunk_frames =
				    std::min<int>(static_cast<int>(discarded.size()) / frame_bytes,
				                  static_cast<int>((static_cast<uint64_t>(frames) * target.freq +
				                                    stream->freq - 1) /
				                                   stream->freq));
				while (queued + bytes > max_bytes && chunk_frames > 0 &&
				       SDL_GetAudioStreamData(stream->sdl, discarded.data(),
				                              chunk_frames * frame_bytes) > 0) {
					queued = std::max(0, SDL_GetAudioStreamQueued(stream->sdl));
				}
			}
		}
		if (queued + bytes > max_bytes) {
			SDL_ClearAudioStream(stream->sdl);
			queued = 0;
		}
	}
	if (!SDL_PutAudioStreamData(stream->sdl, stream->buffer.data(), bytes)) {
		return 0;
	}
	return static_cast<uint64_t>(queued + bytes) * 1000000 / (stream->freq * FRAME_BYTES);
}

} // namespace

Stream* Open(uint32_t freq, bool speaker) {
	if (freq == 0 || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		return nullptr;
	}
	return new Stream {.freq = freq, .speaker = speaker};
}

void Close(Stream* stream) {
	if (stream != nullptr) {
		CloseDevice(stream);
		delete stream;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

bool UsesBluetooth(const Stream* stream) {
	return stream != nullptr && stream->bluetooth != nullptr;
}

uint64_t Queue(Stream* stream, int controller, const void* data, uint32_t frames, uint32_t channels,
               bool is_float, const int* volume, float gain, const float* gains) {
	if (stream == nullptr || data == nullptr || frames == 0 || channels == 0 || volume == nullptr) {
		return 0;
	}
	const auto max_frames =
	    static_cast<uint32_t>(static_cast<uint64_t>(stream->freq) * MAX_QUEUE_MS / 1000);
	const auto first_frame = frames > max_frames ? frames - max_frames : 0;
	frames -= first_frame;
	if (frames == 0) {
		return 0;
	}
	const bool wireless = IsWireless(controller);
	if (!wireless && !CanUseDevice(controller)) {
		CloseDevice(stream);
		stream->next_check = 0; // Look for the device as soon as a DualSense is active again.
		return 0;
	}
	RefreshDevice(stream, controller, wireless);
	if (stream->sdl == nullptr && stream->bluetooth == nullptr) {
		return 0;
	}
	if (stream->speaker) {
		bool routed;
		{
			Common::LockGuard lock(g_mutex);
			routed = SelectSpeaker(controller);
		}
		if (!routed) {
			CloseDevice(stream);
			return 0;
		}
	}
	const uint32_t output_channels = wireless ? 2 : 4;
	const uint32_t channel_offset  = wireless || stream->speaker ? 0 : 2;
	stream->buffer.assign(static_cast<size_t>(frames) * output_channels, 0.0f);
	bool audible = false;
	for (uint32_t frame = 0; frame < frames; frame++) {
		for (uint32_t ch = 0; ch < 2; ch++) {
			const auto src_ch = channels == 1 ? 0 : ch;
			const auto index  = (static_cast<size_t>(first_frame) + frame) * channels + src_ch;
			float      value  = is_float ? static_cast<const float*>(data)[index]
			                             : static_cast<const int16_t*>(data)[index] / 32768.0f;
			value *= volume[src_ch] / 32768.0f * gain * (gains != nullptr ? gains[src_ch] : 1.0f);
			// USB places the speaker in front and the actuators in back; Bluetooth takes stereo.
			stream->buffer[static_cast<size_t>(frame) * output_channels + ch + channel_offset] =
			    value;
			audible |= std::isfinite(value) && std::fabs(value) > 1.0f / 1024;
		}
	}
	const auto queued_us =
	    wireless ? DualSenseBluetooth::Queue(stream->bluetooth, stream->buffer.data(), frames)
	             : QueueUsbAudio(stream, frames);
	if (queued_us == 0) {
		CloseDevice(stream);
		stream->next_check = SDL_GetTicks() + 2000;
		return 0;
	}
	if (audible && !stream->speaker) {
		Common::LockGuard lock(g_mutex);
		SelectController(controller);
		const auto now      = SDL_GetTicks();
		const bool starting = g_haptics_until <= now;
		g_haptics_until =
		    std::max(g_haptics_until, now + std::max<uint64_t>(queued_us / 1000, 250));
		if (starting) {
			ApplyRumble();
		}
	}
	return queued_us;
}

bool SetVibration(int controller, uint8_t large_motor, uint8_t small_motor, uint32_t duration_ms) {
	if (SDL_GetGamepadTypeForID(static_cast<SDL_JoystickID>(controller)) != SDL_GAMEPAD_TYPE_PS5) {
		return false;
	}
	Common::LockGuard lock(g_mutex);
	SelectController(controller);
	g_large_motor  = large_motor;
	g_small_motor  = small_motor;
	g_rumble_until = SDL_GetTicks() + duration_ms;
	ApplyRumble();
	return true;
}

void Shutdown() {
	DualSenseBluetooth::Shutdown();
	Common::LockGuard lock(g_mutex);
	SelectController(-1);
	SelectSpeaker(-1);
}

} // namespace Libs::Controller::DualSenseHaptics
