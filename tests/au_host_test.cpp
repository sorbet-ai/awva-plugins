// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A minimal AU host for the awva plugins (macOS, AudioToolbox). The component is registered in this
// process only (AudioComponentRegister with its bundle's factory function): nothing is installed,
// and no audio device is opened (it only calls AudioUnitRender).
//
//   au_host_test expressive PLUGIN.component COMPONENT_LIB DIR RATE
//     The AU asks for MIDI 2.0 (kAudioUnitProperty_AudioUnitMIDIProtocol). The expressive
//     performances (tests/expressive.h), in render cycles of 128, 1, 33 and random sizes up to
//     1024: per_note as MIDI 2.0 (MusicDeviceMIDIEventList, protocol 2.0: Note On/Off, Poly
//     Pressure, Registered Per-Note Controllers 3 and 74), mpe as MIDI
//     1.0 (MusicDeviceMIDIEvent) and as MIDI 1.0 event lists (protocol 1.0), which the system
//     gives the AU translated to MIDI 2.0 (it asked for 2.0). Each must equal awva_render --ftz
//     --ump's render of its UMP stream (DIR/ref_per_note_RATE.f32, and DIR/ref_mpe_midi2_RATE.f32:
//     the UMP spec's default translation) x 0.25, bit for bit.
//   au_host_test bench PLUGIN.component VOICES
//     As clap_host_test bench, through the AU (MIDI 2.0 event lists): "bench au N LOAD%" for 1, 4
//     and VOICES notes, and "latency au REPORTED MIN MAX" (REPORTED: kAudioUnitProperty_Latency in
//     frames).
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "expressive.h"

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}

AudioComponent component = nullptr;

OSType fourcc(CFStringRef s) {
	char b[8] = {};
	CFStringGetCString(s, b, sizeof b, kCFStringEncodingASCII);
	return (OSType(uint8_t(b[0])) << 24) | (OSType(uint8_t(b[1])) << 16) | (OSType(uint8_t(b[2])) << 8) | OSType(uint8_t(b[3]));
}
// the bundle's component, registered in this process
bool register_component(const char* path) {
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path), static_cast<CFIndex>(std::strlen(path)), true);
	CFBundleRef bundle = CFBundleCreate(nullptr, url);
	CFRelease(url);
	if (!bundle || !CFBundleLoadExecutable(bundle)) return false;
	auto list = static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents")));
	if (!list || CFArrayGetCount(list) < 1) return false;
	auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
	AudioComponentDescription desc = {fourcc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("type")))),
	    fourcc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("subtype")))),
	    fourcc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("manufacturer")))), 0, 0};
	auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
	    CFBundleGetFunctionPointerForName(bundle, static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("factoryFunction")))));
	if (!factory) return false;
	component = AudioComponentRegister(&desc, CFSTR("awva au_host_test"), 0x10000, factory);
	return component != nullptr;
}

// an initialized instance at a rate: stereo float32, non-interleaved, cycles up to 4096 frames
struct Instance {
	AudioUnit unit = nullptr;
	explicit Instance(double rate) {
		if (AudioComponentInstanceNew(component, &unit) != noErr) {
			unit = nullptr;
			return;
		}
		UInt32 max = 4096;
		AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max, sizeof max);
		AudioStreamBasicDescription f = {};
		f.mSampleRate = rate;
		f.mFormatID = kAudioFormatLinearPCM;
		f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
		f.mBytesPerPacket = 4;
		f.mFramesPerPacket = 1;
		f.mBytesPerFrame = 4;
		f.mChannelsPerFrame = 2;
		f.mBitsPerChannel = 32;
		if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) != noErr ||
		    AudioUnitInitialize(unit) != noErr) {
			AudioComponentInstanceDispose(unit);
			unit = nullptr;
		}
	}
	~Instance() {
		if (!unit) return;
		AudioUnitUninitialize(unit);
		AudioComponentInstanceDispose(unit);
	}
};

enum class Path { midi2_list, midi1, midi1_list };

// one event through the path, at its offset in the next render cycle
void send(AudioUnit u, Path path, const expressive::Event& e, const expressive::Packet& p, UInt32 offset) {
	if (path == Path::midi1) {
		uint8_t b[3] = {};
		if (expressive::bytes(e, b)) MusicDeviceMIDIEvent(u, b[0], b[1], b[2], offset);
		return;
	}
	alignas(8) uint8_t buf[256];
	auto* list = reinterpret_cast<MIDIEventList*>(buf);
	MIDIEventPacket* pkt = MIDIEventListInit(list, path == Path::midi2_list ? kMIDIProtocol_2_0 : kMIDIProtocol_1_0);
	const uint32_t words[2] = {p.w0, p.w1};
	pkt = MIDIEventListAdd(list, sizeof buf, pkt, 0, (p.w0 >> 28) == 4 ? 2 : 1, words);
	if (pkt) MusicDeviceMIDIEventList(u, offset, list);
}

// the performance in cycles of `block` (0: random 1..1024, seeded); interleaved stereo out, the
// seconds spent sending and rendering in *spent
std::vector<float> play(const std::vector<expressive::Event>& perf, Path path, uint64_t frames, int block, double rate, double* spent = nullptr) {
	Instance in(rate);
	if (!in.unit) return {};
	// the events and their UMP, one each (mpe as MIDI 1.0 words for the protocol 1.0 lists)
	const std::vector<expressive::Packet> u = expressive::ump(perf);
	std::vector<float> out(frames * 2), l(4096), r(4096);
	std::mt19937 rng(1);
	size_t k = 0;
	uint64_t at = 0;
	if (spent) *spent = 0;
	while (at < frames) {
		const UInt32 n = static_cast<UInt32>(std::min<uint64_t>(block > 0 ? static_cast<uint64_t>(block) : 1 + rng() % 1024, frames - at));
		const auto t0 = std::chrono::steady_clock::now();
		for (; k < perf.size() && perf[k].frame < at + n; k++) send(in.unit, path, perf[k], u[k], static_cast<UInt32>(perf[k].frame - at));
		alignas(16) uint8_t abl_buf[sizeof(AudioBufferList) + sizeof(AudioBuffer)];
		auto* abl = reinterpret_cast<AudioBufferList*>(abl_buf);
		abl->mNumberBuffers = 2;
		abl->mBuffers[0] = {1, n * 4, l.data()};
		abl->mBuffers[1] = {1, n * 4, r.data()};
		AudioTimeStamp ts = {};
		ts.mSampleTime = static_cast<Float64>(at);
		ts.mFlags = kAudioTimeStampSampleTimeValid;
		AudioUnitRenderActionFlags flags = 0;
		if (AudioUnitRender(in.unit, &flags, &ts, 0, n, abl) != noErr) return {};
		if (spent) *spent += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		const auto* lo = static_cast<const float*>(abl->mBuffers[0].mData);
		const auto* ro = static_cast<const float*>(abl->mBuffers[1].mData);
		for (UInt32 i = 0; i < n; i++) {
			out[(at + i) * 2] = lo[i];
			out[(at + i) * 2 + 1] = ro[i];
		}
		at += n;
	}
	return out;
}
std::string blocks_name(int b) { return b > 0 ? std::to_string(b) : std::string("random 1..1024"); }

void expect_ref(const std::vector<float>& out, const std::vector<float>& ref, const std::string& what) {
	const expressive::Diff d = expressive::differ(out, ref);
	char line[400];
	std::snprintf(line, sizeof line, "%s: %zu frames (peak %.3f) bit for bit awva_render --ftz --ump's x 0.25", what.c_str(), ref.size(), d.peak);
	check(!ref.empty() && d.bad == 0 && d.peak > 0.001, line);
	if (d.bad) std::printf("     %zu frames differ, the first at %zu, by up to %.3g (%.1f dBFS)\n", d.bad, d.first, d.max_diff, 20 * std::log10(d.max_diff + 1e-300));
}

int expressive_test(const std::string& dir, int rate) {
	using namespace expressive;
	const std::string r = std::to_string(rate);
	const std::vector<float> ref_note = read_f32(dir + "/ref_per_note_" + r + ".f32"), ref_mpe = read_f32(dir + "/ref_mpe_midi2_" + r + ".f32");
	if (ref_note.empty() || ref_mpe.empty()) {
		std::fprintf(stderr, "au_host_test: no references in %s (awva_render --ump)\n", dir.c_str());
		return 2;
	}
	{
		Instance in(rate);
		if (!in.unit) {
			std::fprintf(stderr, "au_host_test: cannot open the AU\n");
			return 2;
		}
		SInt32 protocol = -1;
		UInt32 size = sizeof protocol;
		AudioUnitGetProperty(in.unit, kAudioUnitProperty_AudioUnitMIDIProtocol, kAudioUnitScope_Global, 0, &protocol, &size);
		check(protocol == kMIDIProtocol_2_0, "the AU asks its host for MIDI 2.0 (kAudioUnitProperty_AudioUnitMIDIProtocol)");
	}
	const std::vector<Event> note = per_note(rate), mp = mpe(rate);
	for (const int block : {128, 1, 33, 0}) {
		const std::string b = r + " Hz, render cycles " + blocks_name(block);
		expect_ref(play(note, Path::midi2_list, ref_note.size(), block, rate), ref_note, "per-note: MIDI 2.0 event lists, " + b);
		expect_ref(play(mp, Path::midi1, ref_mpe.size(), block, rate), ref_mpe, "MPE: MIDI 1.0 (MusicDeviceMIDIEvent), as MIDI 2.0, " + b);
		expect_ref(play(mp, Path::midi1_list, ref_mpe.size(), block, rate), ref_mpe, "MPE: MIDI 1.0 event lists, as MIDI 2.0, " + b);
	}
	return 0;
}

int bench_test(int voices_max) {
	constexpr int rate = 48000;
	constexpr double seconds = 4;
	Float64 latency = -1;
	{
		Instance in(rate);
		if (!in.unit) {
			std::fprintf(stderr, "au_host_test: cannot open the AU\n");
			return 2;
		}
		UInt32 size = sizeof latency;
		AudioUnitGetProperty(in.unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &size);
	}
	const uint64_t frames = static_cast<uint64_t>(seconds * rate);
	for (const int voices : {1, 4, voices_max}) {
		const std::vector<expressive::Event> perf = expressive::bench(voices, rate, seconds);
		double best = 1e9;
		for (int run = 0; run < 5; run++) {
			double spent = 0;
			(void)play(perf, Path::midi2_list, frames, 128, rate, &spent);
			best = std::fmin(best, spent);
		}
		std::printf("bench au %d %.2f\n", voices, 100 * best / seconds);
	}
	int64_t lo = 1 << 30, hi = -1;
	for (int phase = 0; phase < 96; phase += 5) {
		const uint64_t on = 4800 + static_cast<uint64_t>(phase);
		const int64_t n = expressive::onset(play(expressive::one_note(on), Path::midi2_list, on + 4800, 128, rate), on);
		if (n < 0) {
			lo = n, hi = n;
			break;
		}
		lo = std::min(lo, n), hi = std::max(hi, n);
	}
	std::printf("latency au %lld %lld %lld\n", static_cast<long long>(std::llround(latency * rate)), static_cast<long long>(lo), static_cast<long long>(hi));
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: au_host_test expressive|bench PLUGIN.component [...]\n");
		return 2;
	}
	const std::string mode = argv[1];
	if (!register_component(argv[2])) {
		std::fprintf(stderr, "au_host_test: cannot load and register %s\n", argv[2]);
		return 2;
	}
	int rc = 0;
	if (mode == "expressive" && argc >= 6) rc = expressive_test(argv[4], std::atoi(argv[5]));
	else if (mode == "bench" && argc >= 4) rc = bench_test(std::atoi(argv[3]));
	else {
		std::fprintf(stderr, "au_host_test: unknown mode %s, or arguments missing\n", mode.c_str());
		return 2;
	}
	if (rc) return rc;
	std::printf(failures ? "au_host_test: %d FAILED\n" : "au_host_test: all passed\n", failures);
	return failures ? 1 : 0;
}
