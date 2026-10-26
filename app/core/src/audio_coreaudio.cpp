// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// CoreAudio: the default output unit (it follows the system's output device and volume), float
// non-interleaved stereo at the device's rate, a 256-frame buffer asked for.
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
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

class CoreAudioOut : public AudioOut {
public:
	~CoreAudioOut() override { stop(); }

	int probe_rate() override {
		Float64 rate = 0;
		UInt32 size = sizeof rate;
		AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
		const AudioObjectID dev = default_device();
		if (dev == kAudioObjectUnknown || AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &rate) != noErr) return 0;
		return static_cast<int>(rate);
	}

	bool start(AudioCallback* cb) override {
		stop();
		cb_ = cb;
		std::lock_guard<std::mutex> g(m_);
		info_ = AudioInfo{};
		info_.backend = "CoreAudio";
		AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
		AudioComponent comp = AudioComponentFindNext(nullptr, &d);
		OSStatus st = comp ? AudioComponentInstanceNew(comp, &unit_) : -1;
		const int rate = probe_rate() > 0 ? probe_rate() : 48000;
		const AudioObjectID dev = default_device();
		if (dev != kAudioObjectUnknown) {
			CFStringRef name = nullptr;
			UInt32 size = sizeof name;
			AudioObjectPropertyAddress a = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
			if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &name) == noErr && name) {
				char b[256];
				if (CFStringGetCString(name, b, sizeof b, kCFStringEncodingUTF8)) info_.device = b;
				CFRelease(name);
			}
			UInt32 frames = 256;
			AudioObjectPropertyAddress fa = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, 0 /* (main) */};
			AudioObjectSetPropertyData(dev, &fa, 0, nullptr, sizeof frames, &frames);
			size = sizeof frames;
			if (AudioObjectGetPropertyData(dev, &fa, 0, nullptr, &size, &frames) == noErr) info_.buffer = static_cast<int>(frames);
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
	static OSStatus callback(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 n, AudioBufferList* io) {
		auto* s = static_cast<CoreAudioOut*>(self);
		float* l = static_cast<float*>(io->mBuffers[0].mData);
		float* r = io->mNumberBuffers > 1 ? static_cast<float*>(io->mBuffers[1].mData) : s->spare_;
		const uint64_t t = ts && (ts->mFlags & kAudioTimeStampHostTimeValid) ? host_to_ns(ts->mHostTime) : now_ns();
		for (UInt32 done = 0; done < n;) {
			const UInt32 k = std::min<UInt32>(n - done, AudioCallback::kMaxFrames);
			s->cb_->render(l + done, r + done, k, t);
			done += k;
		}
		return noErr;
	}
	mutable std::mutex m_;
	AudioInfo info_;
	AudioUnit unit_ = nullptr;
	AudioCallback* cb_ = nullptr;
	float spare_[AudioCallback::kMaxFrames * 2];
};

} // namespace

std::unique_ptr<AudioOut> make_device_audio() { return std::make_unique<CoreAudioOut>(); }

} // namespace awva_app
#endif
