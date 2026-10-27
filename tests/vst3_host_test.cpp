// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A minimal VST3 host for the awva plugins (the VST3 SDK's hosting module). It opens no audio
// device: it only calls process().
//
//   vst3_host_test programs PLUGIN.vst3 COMPONENT_LIB
//     The VST3's preset list (the program list of its root unit, as Cubase reads it) holds the
//     component's factory presets, by name, in their order.
//   vst3_host_test expressive PLUGIN.vst3 COMPONENT_LIB DIR RATE
//     What the VST3 offers a host for expressive input: the tuning and brightness note
//     expressions (INoteExpressionController), and pitch bend, channel pressure and CC74 on all 16
//     channels (IMidiMapping). Then the expressive performances (tests/expressive.h), in host
//     blocks of 128, 1, 33 and random sizes up to 400: per_note as VST3 notes, poly pressure and
//     the tuning and brightness note expressions, and mpe as VST3 notes with the channels' bend,
//     pressure and CC74 as the parameter changes of their MIDI controllers (IMidiMapping), as a
//     VST3 host plays MPE. Each must equal awva_render --ftz --ump's render of its UMP stream
//     (DIR/ref_per_note_RATE.f32, DIR/ref_mpe_vst3_RATE.f32), bit for bit.
//   vst3_host_test rates PLUGIN.vst3
//     The sample rates a host may set (the VST3 SDK validator's "Process Format" list): a rate the
//     plugin takes is accepted by setupProcessing() and then activates and processes; one it
//     refuses (the engines run at 11.025 to 192 kHz) is refused by setupProcessing() already.
//   vst3_host_test bench PLUGIN.vst3 VOICES
//     As clap_host_test bench, through the VST3: "bench vst3 N LOAD%" for 1, 4 and VOICES (the
//     plugin's voices) notes, and "latency vst3 REPORTED MIN MAX".
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // (nsc/load.h includes windows.h)
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "expressive.h"
#include "nsc/load.h"
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

using namespace Steinberg;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}

// the main thread's events for 100 ms, as a host's idle: the plugin's timer (clap-wrapper's onIdle)
// publishes its preset list once its preset index is built
void idle() {
#if defined(_WIN32)
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	while (std::chrono::steady_clock::now() < until) {
		MSG m;
		while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessage(&m);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
#elif defined(__APPLE__)
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
}

VST3::Hosting::Module::Ptr module;
Vst::HostApplication host;

// one instance of the plugin: its component (the processor) and its controller
struct Instance {
	IPtr<Vst::IComponent> component;
	IPtr<Vst::IEditController> controller;
	bool separate = false; // (a controller of its own, else the component is its controller)
	bool active = false;
	Instance() {
		const auto factory = module->getFactory();
		for (const auto& ci : factory.classInfos())
			if (ci.category() == kVstAudioEffectClass) {
				component = factory.createInstance<Vst::IComponent>(ci.ID());
				break;
			}
		if (!component || component->initialize(&host) != kResultOk) {
			component = nullptr;
			return;
		}
		TUID cid;
		if (component->getControllerClassId(cid) == kResultOk) {
			controller = factory.createInstance<Vst::IEditController>(VST3::UID::fromTUID(cid));
			separate = static_cast<bool>(controller);
		}
		if (!controller) controller = FUnknownPtr<Vst::IEditController>(component);
		if (separate && controller && controller->initialize(&host) != kResultOk) controller = nullptr;
	}
	~Instance() {
		if (active) {
			if (FUnknownPtr<Vst::IAudioProcessor> p(component); p) p->setProcessing(false);
			component->setActive(false);
		}
		if (separate && controller) controller->terminate();
		controller = nullptr;
		if (component) component->terminate();
	}
	bool ok() const { return component && controller; }
	// active at a rate, the output and the note input buses on, processing
	bool start(double rate, int32 max_block) {
		FUnknownPtr<Vst::IAudioProcessor> processor(component);
		Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, max_block, rate};
		if (!processor || processor->setupProcessing(setup) != kResultOk) return false;
		component->activateBus(Vst::kAudio, Vst::kOutput, 0, true);
		component->activateBus(Vst::kEvent, Vst::kInput, 0, true);
		if (component->setActive(true) != kResultOk) return false;
		active = true;
		processor->setProcessing(true);
		return true;
	}
};

// --- programs ------------------------------------------------------------------------------------------
int programs_test(const char* lib_path) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "vst3_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	std::vector<std::string> want;
	for (int i = 0; i < lib.c->desc->num_presets; i++) want.push_back(lib.c->desc->presets[i].name);
	nsc_unload(&lib);
	Instance in;
	// active, as in a host's project (the plugin's idle timer runs while it is active)
	if (!in.ok() || !in.start(48000, 512)) {
		std::fprintf(stderr, "vst3_host_test: no component, or not activated\n");
		return 2;
	}
	FUnknownPtr<Vst::IUnitInfo> units(in.controller);
	if (!units) {
		check(false, "the controller has IUnitInfo");
		return 0;
	}
	// the root unit's program list; the plugin fills it once its preset index is built
	Vst::ProgramListID list = Vst::kNoProgramListId;
	Vst::ProgramListInfo info{};
	for (int tries = 0; tries < 50; tries++) {
		for (int32 u = 0; u < units->getUnitCount(); u++) {
			Vst::UnitInfo ui{};
			if (units->getUnitInfo(u, ui) == kResultOk && ui.id == Vst::kRootUnitId) list = ui.programListId;
		}
		info = {};
		for (int32 k = 0; k < units->getProgramListCount(); k++) {
			Vst::ProgramListInfo pi{};
			if (units->getProgramListInfo(k, pi) == kResultOk && pi.id == list) info = pi;
		}
		if (want.empty() || info.programCount >= static_cast<int32>(want.size())) break;
		idle();
	}
	std::vector<std::string> got;
	for (int32 i = 0; list != Vst::kNoProgramListId && i < info.programCount; i++) {
		Vst::String128 name{};
		if (units->getProgramName(list, i, name) == kResultOk) got.push_back(Vst::StringConvert::convert(name));
	}
	const bool ok = want.empty() ? got.size() <= 1 : got == want;
	char what[256];
	std::snprintf(what, sizeof what, "the VST3's preset list (root unit, program list %d): %zu presets%s", static_cast<int>(list), got.size(),
	    want.empty() ? " (none in the component)" : ", the component's factory presets in their order");
	check(ok, what);
	if (!ok)
		for (size_t i = 0; i < got.size() || i < want.size(); i++)
			std::printf("     %zu: %s | %s\n", i, i < got.size() ? got[i].c_str() : "-", i < want.size() ? want[i].c_str() : "-");
	return 0;
}

// --- playing -------------------------------------------------------------------------------------------
// a performance of tests/expressive.h as VST3 input: events, and parameter changes of MIDI controllers
struct Input {
	uint64_t frame;
	bool is_event;
	Vst::Event event;   // (is_event)
	Vst::ParamID param; // (else)
	Vst::ParamValue value;
};
std::vector<Input> vst3_input(const std::vector<expressive::Event>& perf, Vst::IMidiMapping* mm) {
	using namespace expressive;
	std::vector<Input> v;
	for (const Event& e : perf) {
		Input in{e.frame, true, {}, 0, 0};
		Vst::Event& x = in.event;
		x.busIndex = 0;
		x.flags = Vst::Event::kIsLive;
		switch (e.kind) {
		case kNoteOn:
			x.type = Vst::Event::kNoteOnEvent;
			x.noteOn = {e.channel, e.key, 0.0f, static_cast<float>(velocity(e)), 0, e.id};
			break;
		case kNoteOff:
			x.type = Vst::Event::kNoteOffEvent;
			x.noteOff = {e.channel, e.key, static_cast<float>(velocity(e)), e.id, 0.0f};
			break;
		case kPressure:
			x.type = Vst::Event::kPolyPressureEvent;
			x.polyPressure = {e.channel, e.key, static_cast<float>(unit(e)), e.id};
			break;
		case kTuning:
		case kBrightness:
			x.type = Vst::Event::kNoteExpressionValueEvent;
			x.noteExpressionValue = {e.kind == kTuning ? Vst::kTuningTypeID : Vst::kBrightnessTypeID, e.id, e.kind == kTuning ? vst3_tuning(e) : unit(e)};
			break;
		default: { // a channel's controller: a parameter change of its MIDI controller
			const Vst::CtrlNumber ctrl = static_cast<Vst::CtrlNumber>(e.kind == kBend ? Vst::kPitchBend : e.kind == kChanPressure ? Vst::kAfterTouch : e.cc);
			Vst::ParamID id = Vst::kNoParamId;
			if (!mm || mm->getMidiControllerAssignment(0, e.channel, ctrl, id) != kResultTrue) continue;
			in.is_event = false;
			in.param = id;
			in.value = e.kind == kBend ? e.value / 16383.0 : e.value / 127.0;
			break;
		}
		}
		v.push_back(in);
	}
	return v;
}

// host blocks: n > 0 that many frames, 0 random 1..max_random (seeded). Interleaved stereo out; the
// seconds spent in process() in *spent
std::vector<float> play(Instance& in, const std::vector<Input>& input, uint64_t frames, int block, double rate, int max_random = 400,
    double* spent = nullptr) {
	FUnknownPtr<Vst::IAudioProcessor> processor(in.component);
	std::vector<float> out(frames * 2);
	std::vector<float> l(4096), r(4096);
	std::mt19937 rng(1);
	Vst::EventList events(8192), out_events(8192);
	Vst::ParameterChanges changes(4096), out_changes(4096);
	size_t k = 0;
	uint64_t at = 0;
	if (spent) *spent = 0;
	while (at < frames) {
		const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(block > 0 ? static_cast<uint64_t>(block) : 1 + rng() % max_random, frames - at));
		events.clear();
		changes.clearQueue();
		out_events.clear();
		out_changes.clearQueue();
		for (; k < input.size() && input[k].frame < at + n; k++) {
			const int32 offset = static_cast<int32>(input[k].frame - at);
			if (input[k].is_event) {
				Vst::Event e = input[k].event;
				e.sampleOffset = offset;
				events.addEvent(e);
			} else {
				int32 qi = 0, pi = 0;
				if (Vst::IParamValueQueue* q = changes.addParameterData(input[k].param, qi)) q->addPoint(offset, input[k].value, pi);
			}
		}
		float* chans[2] = {l.data(), r.data()};
		Vst::AudioBusBuffers bus{};
		bus.numChannels = 2;
		bus.channelBuffers32 = chans;
		Vst::ProcessData d{};
		d.processMode = Vst::kRealtime;
		d.symbolicSampleSize = Vst::kSample32;
		d.numSamples = static_cast<int32>(n);
		d.numOutputs = 1;
		d.outputs = &bus;
		d.inputParameterChanges = &changes;
		d.outputParameterChanges = &out_changes;
		d.inputEvents = &events;
		d.outputEvents = &out_events;
		const auto t0 = std::chrono::steady_clock::now();
		processor->process(d);
		if (spent) *spent += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		for (uint32_t i = 0; i < n; i++) {
			out[(at + i) * 2] = l[i];
			out[(at + i) * 2 + 1] = r[i];
		}
		at += n;
	}
	return out;
}
std::vector<float> play_new(const std::vector<expressive::Event>& perf, uint64_t frames, int block, double rate, int max_random = 400,
    double* spent = nullptr) {
	Instance in;
	if (!in.ok() || !in.start(rate, 4096)) return {};
	FUnknownPtr<Vst::IMidiMapping> mm(in.controller);
	return play(in, vst3_input(perf, mm.get()), frames, block, rate, max_random, spent);
}
std::string blocks_name(int b, int max_random) { return b > 0 ? std::to_string(b) : "random 1.." + std::to_string(max_random); }
bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// --- expressive ----------------------------------------------------------------------------------------
void expect_ref(const std::vector<float>& out, const std::vector<float>& ref, const std::string& what) {
	const expressive::Diff d = expressive::differ(out, ref);
	char line[400];
	std::snprintf(line, sizeof line, "%s: %zu frames (peak %.3f) bit for bit awva_render --ftz --ump's", what.c_str(), ref.size(), d.peak);
	check(!ref.empty() && d.bad == 0 && d.peak > 0.001, line);
	if (d.bad) std::printf("     %zu frames differ, the first at %zu, by up to %.3g (%.1f dBFS)\n", d.bad, d.first, d.max_diff, 20 * std::log10(d.max_diff + 1e-300));
}
int expressive_test(const char* lib_path, const std::string& dir, int rate) {
	using namespace expressive;
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "vst3_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	const bool reads_timbre = (lib.c->desc->inputs & (1u << 2)) != 0; // (engine.h in_timbre)
	nsc_unload(&lib);
	const std::string r = std::to_string(rate);
	const std::vector<float> ref_note = read_f32(dir + "/ref_per_note_" + r + ".f32"), ref_mpe = read_f32(dir + "/ref_mpe_vst3_" + r + ".f32");
	if (ref_note.empty() || ref_mpe.empty()) {
		std::fprintf(stderr, "vst3_host_test: no references in %s (awva_render --ump)\n", dir.c_str());
		return 2;
	}
	{ // what the plugin declares
		Instance in;
		if (!in.ok()) {
			std::fprintf(stderr, "vst3_host_test: no component\n");
			return 2;
		}
		FUnknownPtr<Vst::INoteExpressionController> ne(in.controller);
		bool tuning = false, brightness = false;
		const int32 count = ne ? ne->getNoteExpressionCount(0, 0) : 0;
		for (int32 i = 0; i < count; i++) {
			Vst::NoteExpressionTypeInfo info{};
			if (ne->getNoteExpressionInfo(0, 0, i, info) != kResultOk) continue;
			tuning = tuning || info.typeId == Vst::kTuningTypeID;
			brightness = brightness || info.typeId == Vst::kBrightnessTypeID;
		}
		check(tuning && brightness, "the note expressions tuning and brightness (INoteExpressionController: " + std::to_string(count) + " types)");
		FUnknownPtr<Vst::IMidiMapping> mm(in.controller);
		int mapped = 0;
		for (int16 ch = 0; mm && ch < 16; ch++)
			for (const Vst::CtrlNumber c : {static_cast<Vst::CtrlNumber>(Vst::kPitchBend), static_cast<Vst::CtrlNumber>(Vst::kAfterTouch), static_cast<Vst::CtrlNumber>(74)}) {
				Vst::ParamID id = Vst::kNoParamId;
				mapped += mm->getMidiControllerAssignment(0, ch, c, id) == kResultTrue;
			}
		check(mapped == 48, "pitch bend, channel pressure and CC74 on all 16 channels (IMidiMapping: " + std::to_string(mapped) + " of 48)");
	}
	const std::vector<Event> note = per_note(rate), mp = mpe(rate);
	for (const int block : {128, 1, 33, 0}) {
		const std::string b = r + " Hz, host blocks " + blocks_name(block, 400);
		expect_ref(play_new(note, ref_note.size(), block, rate), ref_note, "per-note: VST3 notes, poly pressure, tuning and brightness note expressions, " + b);
		expect_ref(play_new(mp, ref_mpe.size(), block, rate), ref_mpe, "MPE: VST3 notes, the channels' bend, pressure and CC74 (IMidiMapping), " + b);
	}
	for (const Kind k : {kPressure, kTuning, kBrightness}) {
		std::vector<Event> without;
		for (const Event& e : note)
			if (e.kind != k) without.push_back(e);
		const char* name = k == kPressure ? "poly pressure" : k == kTuning ? "tuning" : "brightness";
		const bool differs = !same_bits(play_new(without, ref_note.size(), 128, rate), play_new(note, ref_note.size(), 128, rate));
		if (k != kBrightness || reads_timbre) check(differs, std::string("per-note: the ") + name + " events change the output");
		else std::printf("info the engine doesn't read timbre: the brightness note expressions %s the output\n", differs ? "change" : "don't change");
	}
	return 0;
}

// --- bench ---------------------------------------------------------------------------------------------
int bench_test(int voices_max) {
	constexpr int rate = 48000;
	constexpr double seconds = 4;
	uint32 reported = 0;
	{
		Instance in;
		if (!in.ok() || !in.start(rate, 128)) {
			std::fprintf(stderr, "vst3_host_test: no component, or not activated\n");
			return 2;
		}
		FUnknownPtr<Vst::IAudioProcessor> p(in.component);
		reported = p->getLatencySamples();
	}
	const uint64_t frames = static_cast<uint64_t>(seconds * rate);
	for (const int voices : {1, 4, voices_max}) {
		const std::vector<expressive::Event> perf = expressive::bench(voices, rate, seconds);
		double best = 1e9;
		for (int run = 0; run < 5; run++) {
			double spent = 0;
			(void)play_new(perf, frames, 128, rate, 400, &spent);
			best = std::fmin(best, spent);
		}
		std::printf("bench vst3 %d %.2f\n", voices, 100 * best / seconds);
	}
	int64_t lo = 1 << 30, hi = -1;
	for (int phase = 0; phase < 96; phase += 5) {
		const uint64_t on = 4800 + static_cast<uint64_t>(phase);
		const int64_t n = expressive::onset(play_new(expressive::one_note(on), on + 4800, 128, rate), on);
		if (n < 0) {
			lo = n, hi = n;
			break;
		}
		lo = std::min(lo, n), hi = std::max(hi, n);
	}
	std::printf("latency vst3 %u %lld %lld\n", reported, static_cast<long long>(lo), static_cast<long long>(hi));
	return 0;
}

} // namespace

// --- rates ------------------------------------------------------------------------------------------------
int rates_test() {
	Instance in;
	if (!in.ok()) return check(false, "rates: the plugin is created"), 1;
	FUnknownPtr<Vst::IAudioProcessor> processor(in.component);
	const double rates[] = {22050, 32000, 44100, 48000, 88200, 96000, 192000, 384000, 1234.5678, 12345.678, 123456.78, 1234567.8, 11025, 192001};
	std::vector<float> l(512), r(512);
	for (const double rate : rates) {
		const bool engine_rate = rate >= 11025 && rate <= 192000; // (the engines' own: init() refuses others)
		Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, 512, rate};
		const bool setup_ok = processor->setupProcessing(setup) == kResultOk;
		bool ok = setup_ok == engine_rate;
		if (setup_ok) {
			in.component->activateBus(Vst::kAudio, Vst::kOutput, 0, true);
			const bool active = in.component->setActive(true) == kResultOk;
			ok = ok && active;
			if (active) {
				processor->setProcessing(true);
				float* chans[2] = {l.data(), r.data()};
				Vst::AudioBusBuffers out;
				out.numChannels = 2;
				out.channelBuffers32 = chans;
				Vst::ProcessData data;
				data.numSamples = 512;
				data.symbolicSampleSize = Vst::kSample32;
				data.numOutputs = 1;
				data.outputs = &out;
				ok = ok && processor->process(data) == kResultOk;
				processor->setProcessing(false);
				in.component->setActive(false);
			}
		}
		char b[200];
		std::snprintf(b, sizeof b, "rates: %.4f Hz %s", rate, engine_rate ? "set up, activated and processed" : "refused by setupProcessing()");
		check(ok, b);
	}
	// (the last rate was refused: the setup before it stays, and activates)
	const bool again = in.component->setActive(true) == kResultOk;
	if (again) in.component->setActive(false);
	check(again, "rates: after a refused rate, the setup before it activates");
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: vst3_host_test programs|expressive|rates|bench PLUGIN.vst3 [...]\n");
		return 2;
	}
	const std::string mode = argv[1];
	std::string err;
	module = VST3::Hosting::Module::create(argv[2], err);
	if (!module) {
		std::fprintf(stderr, "vst3_host_test: cannot load %s: %s\n", argv[2], err.c_str());
		return 2;
	}
	int rc = 0;
	if (mode == "programs" && argc >= 4) rc = programs_test(argv[3]);
	else if (mode == "expressive" && argc >= 6) rc = expressive_test(argv[3], argv[4], std::atoi(argv[5]));
	else if (mode == "bench" && argc >= 4) rc = bench_test(std::atoi(argv[3]));
	else if (mode == "rates") rc = rates_test();
	else {
		std::fprintf(stderr, "vst3_host_test: unknown mode %s, or arguments missing\n", mode.c_str());
		return 2;
	}
	module = nullptr;
	if (rc) return rc;
	std::printf(failures ? "vst3_host_test: %d FAILED\n" : "vst3_host_test: all passed\n", failures);
	return failures ? 1 : 0;
}
