// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// CoreAudio: the default output unit (it follows the system's output device and volume), or the
// output chosen (its device UID) through the HAL output unit; one that isn't there plays the default
// and says so. Float non-interleaved stereo at the device's rate, a 128-frame buffer asked for. The
// bus from the device's transport type (Bluetooth, Bluetooth LE, USB, else wired). A listener on
// the device list and the default output tells the core to restart on them.
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "buffer_policy.h"
#include "platform.h"

namespace awva_app {

namespace {

uint64_t host_to_ns(uint64_t t) {
	static const mach_timebase_info_data_t tb = [] {
		mach_timebase_info_data_t i;
		mach_timebase_info(&i);
		return i;
	}();
	return t * tb.numer / tb.denom;
}

AudioObjectID default_device() {
	AudioObjectID dev = kAudioObjectUnknown;
	UInt32 size = sizeof dev;
	AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
	AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &dev);
	return dev;
}

// a device's string property (its name, its UID), "" unreadable
std::string device_string(AudioObjectID dev, AudioObjectPropertySelector sel) {
	std::string s;
	CFStringRef v = nullptr;
	UInt32 size = sizeof v;
	AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
	if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &v) == noErr && v) {
		char b[512];
		if (CFStringGetCString(v, b, sizeof b, kCFStringEncodingUTF8)) s = b;
		CFRelease(v);
	}
	return s;
}

// the device's bus, from its transport type
const char* device_bus(AudioObjectID dev) {
	UInt32 t = 0, size = sizeof t;
	AudioObjectPropertyAddress a = {kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
	if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &t) != noErr) return bus_name(Bus::Other);
	if (t == kAudioDeviceTransportTypeBluetooth || t == kAudioDeviceTransportTypeBluetoothLE) return bus_name(Bus::Bluetooth);
	if (t == kAudioDeviceTransportTypeUSB) return bus_name(Bus::Usb);
	return bus_name(Bus::Other);
}

// every device with output channels
std::vector<AudioObjectID> output_devices() {
	std::vector<AudioObjectID> out;
	AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr || size == 0) return out;
	std::vector<AudioObjectID> all(size / sizeof(AudioObjectID));
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, all.data()) != noErr) return out;
	all.resize(size / sizeof(AudioObjectID));
	for (const AudioObjectID dev : all) {
		AudioObjectPropertyAddress c = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, 0 /* (main) */};
		UInt32 n = 0;
		if (AudioObjectGetPropertyDataSize(dev, &c, 0, nullptr, &n) != noErr || n == 0) continue;
		std::vector<uint8_t> buf(n);
		auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
		if (AudioObjectGetPropertyData(dev, &c, 0, nullptr, &n, list) != noErr) continue;
		UInt32 channels = 0;
		for (UInt32 k = 0; k < list->mNumberBuffers; k++) channels += list->mBuffers[k].mNumberChannels;
		if (channels > 0) out.push_back(dev);
	}
	return out;
}

// the chosen output by its UID (missing: it isn't there), else the default
AudioObjectID find_output(const std::string& uid, bool* missing) {
	*missing = false;
	if (!uid.empty()) {
		for (const AudioObjectID dev : output_devices())
			if (device_string(dev, kAudioDevicePropertyDeviceUID) == uid) return dev;
		*missing = true;
	}
	return default_device();
}

const AudioObjectPropertyAddress kWatched[] = {
    {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 /* (main) */},
    {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, 0 /* (main) */},
};

class CoreAudioOut : public AudioOut {
public:
	CoreAudioOut() {
		for (const auto& a : kWatched) AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, &CoreAudioOut::changed, this);
	}
	~CoreAudioOut() override {
		stop();
		for (const auto& a : kWatched) AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a, &CoreAudioOut::changed, this);
	}

	int probe_rate() override {
		bool missing = false;
		return rate_of(find_output(config_.device, &missing));
	}

	std::vector<OutputDevice> devices() override {
		std::vector<OutputDevice> v;
		const AudioObjectID def = default_device();
		for (const AudioObjectID dev : output_devices())
			v.push_back({device_string(dev, kAudioDevicePropertyDeviceUID), device_string(dev, kAudioObjectPropertyName), device_bus(dev), dev == def});
		return v;
	}
	bool take_default_changed() override { return default_changed_.exchange(false); }
	bool take_devices_changed() override { return devices_changed_.exchange(false); }

	bool start(AudioCallback* cb) override {
		stop();
		cb_ = cb;
		std::lock_guard<std::mutex> g(m_);
		info_ = AudioInfo{};
		info_.backend = "CoreAudio";
		bool missing = false;
		const AudioObjectID dev = find_output(config_.device, &missing);
		// (a chosen output: the HAL unit on that device; else the default unit, which follows the default)
		const bool chosen = !config_.device.empty() && !missing;
		info_.missing = missing;
		AudioComponentDescription d = {kAudioUnitType_Output, chosen ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput,
		    kAudioUnitManufacturer_Apple, 0, 0};
		AudioComponent comp = AudioComponentFindNext(nullptr, &d);
		OSStatus st = comp ? AudioComponentInstanceNew(comp, &unit_) : -1;
		if (st == noErr && chosen) {
			AudioObjectID id = dev;
			st = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, sizeof id);
		}
		const int rate = rate_of(dev) > 0 ? rate_of(dev) : 48000;
		if (dev != kAudioObjectUnknown) {
			info_.device = device_string(dev, kAudioObjectPropertyName);
			info_.endpoint = device_string(dev, kAudioDevicePropertyDeviceUID);
			info_.bus = device_bus(dev);
			UInt32 frames = 128, size = sizeof frames;
			AudioObjectPropertyAddress fa = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
			AudioObjectSetPropertyData(dev, &fa, 0, nullptr, sizeof frames, &frames);
			if (AudioObjectGetPropertyData(dev, &fa, 0, nullptr, &size, &frames) == noErr) info_.buffer = info_.period = static_cast<int>(frames);
			// the device's own latency and safety offset (output scope), as the driver reports them
			UInt32 lat = 0, safety = 0;
			size = sizeof lat;
			AudioObjectPropertyAddress la = {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, 0 /* (main) */};
			const bool l1 = AudioObjectGetPropertyData(dev, &la, 0, nullptr, &size, &lat) == noErr;
			la.mSelector = kAudioDevicePropertySafetyOffset;
			size = sizeof safety;
			const bool l2 = AudioObjectGetPropertyData(dev, &la, 0, nullptr, &size, &safety) == noErr;
			if (l1 || l2) info_.stream_ms = static_cast<float>((lat + safety) * 1000.0 / rate);
		}
		if (st == noErr) {
			AudioStreamBasicDescription f = {};
			f.mSampleRate = rate;
			f.mFormatID = kAudioFormatLinearPCM;
			f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
			f.mBytesPerPacket = 4;
			f.mFramesPerPacket = 1;
			f.mBytesPerFrame = 4;
			f.mChannelsPerFrame = 2;
			f.mBitsPerChannel = 32;
			st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
		}
		if (st == noErr) {
			AURenderCallbackStruct r = {&CoreAudioOut::callback, this};
			st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &r, sizeof r);
		}
		if (st == noErr) st = AudioUnitInitialize(unit_);
		if (st == noErr) st = AudioOutputUnitStart(unit_);
		info_.rate = rate;
		if (st != noErr) {
			info_.state = AWVA_AUDIO_FAILED;
			info_.message = "the audio output can't be opened (" + std::to_string(static_cast<int>(st)) + ")";
			if (unit_) {
				AudioComponentInstanceDispose(unit_);
				unit_ = nullptr;
			}
			return false;
		}
		info_.state = AWVA_AUDIO_RUNNING;
		return true;
	}
	void stop() override {
		if (unit_) {
			AudioOutputUnitStop(unit_);
			AudioUnitUninitialize(unit_);
			AudioComponentInstanceDispose(unit_);
			unit_ = nullptr;
		}
	}
	AudioInfo info() const override {
		std::lock_guard<std::mutex> g(m_);
		return info_;
	}

private:
	static int rate_of(AudioObjectID dev) {
		Float64 rate = 0;
		UInt32 size = sizeof rate;
		AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
		if (dev == kAudioObjectUnknown || AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &rate) != noErr) return 0;
		return static_cast<int>(rate);
	}
	// (CoreAudio's notification thread)
	static OSStatus changed(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* a, void* self) {
		auto* s = static_cast<CoreAudioOut*>(self);
		for (UInt32 k = 0; k < n; k++) {
			if (a[k].mSelector == kAudioHardwarePropertyDevices) s->devices_changed_.store(true);
			if (a[k].mSelector == kAudioHardwarePropertyDefaultOutputDevice) s->default_changed_.store(true);
		}
		return noErr;
	}
	static OSStatus callback(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 n, AudioBufferList* io) {
		auto* s = static_cast<CoreAudioOut*>(self);
		float* l = static_cast<float*>(io->mBuffers[0].mData);
		float* r = io->mNumberBuffers > 1 ? static_cast<float*>(io->mBuffers[1].mData) : s->spare_;
		const uint64_t t = ts && (ts->mFlags & kAudioTimeStampHostTimeValid) ? host_to_ns(ts->mHostTime) : now_ns();
		for (UInt32 done = 0; done < n;) {
			const UInt32 k = std::min<UInt32>(n - done, AudioCallback::kMaxFrames);
			s->cb_->render(l + done, r + done, k, t);
			s->cb_->tap(l + done, r + done, k);
			done += k;
		}
		return noErr;
	}
	mutable std::mutex m_;
	AudioInfo info_;
	AudioUnit unit_ = nullptr;
	AudioCallback* cb_ = nullptr;
	std::atomic<bool> default_changed_{false}, devices_changed_{false};
	float spare_[AudioCallback::kMaxFrames * 2];
};

} // namespace

std::unique_ptr<AudioOut> make_device_audio() { return std::make_unique<CoreAudioOut>(); }

} // namespace awva_app
#endif
