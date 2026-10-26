// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// vst3_programs PLUGIN.vst3 COMPONENT_LIB: the VST3's preset list (the program list of its root
// unit, as Cubase reads it) holds the component's factory presets, by name, in their order. A
// minimal VST3 host (the VST3 SDK's hosting module); it opens no audio device.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "nsc/load.h"
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

using namespace Steinberg;

// the main thread's events for 100 ms, as a host's idle: the plugin's timer (clap-wrapper's onIdle)
// publishes its preset list once its preset index is built
static void idle() {
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

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: vst3_programs PLUGIN.vst3 COMPONENT_LIB\n");
		return 2;
	}
	nsc_library lib;
	if (nsc_load(argv[2], &lib) != 0) {
		std::fprintf(stderr, "vst3_programs: cannot load the component %s\n", argv[2]);
		return 2;
	}
	std::vector<std::string> want;
	for (int i = 0; i < lib.c->desc->num_presets; i++) want.push_back(lib.c->desc->presets[i].name);
	nsc_unload(&lib);

	std::string err;
	auto module = VST3::Hosting::Module::create(argv[1], err);
	if (!module) {
		std::fprintf(stderr, "vst3_programs: cannot load %s: %s\n", argv[1], err.c_str());
		return 2;
	}
	Vst::HostApplication host;
	const auto factory = module->getFactory();
	IPtr<Vst::IComponent> component;
	for (const auto& ci : factory.classInfos())
		if (ci.category() == kVstAudioEffectClass) {
			component = factory.createInstance<Vst::IComponent>(ci.ID());
			break;
		}
	if (!component || component->initialize(&host) != kResultOk) {
		std::fprintf(stderr, "vst3_programs: no component\n");
		return 2;
	}
	TUID cid;
	IPtr<Vst::IEditController> controller;
	bool separate = false; // (a controller of its own, else the component is its controller)
	if (component->getControllerClassId(cid) == kResultOk) {
		controller = factory.createInstance<Vst::IEditController>(VST3::UID::fromTUID(cid));
		separate = static_cast<bool>(controller);
	}
	if (!controller) controller = FUnknownPtr<Vst::IEditController>(component);
	if (!controller || (separate && controller->initialize(&host) != kResultOk)) {
		std::fprintf(stderr, "vst3_programs: no controller\n");
		return 2;
	}
	// active, as in a host's project (the plugin's idle timer runs while it is active)
	FUnknownPtr<Vst::IAudioProcessor> processor(component);
	Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, 512, 48000.0};
	if (!processor || processor->setupProcessing(setup) != kResultOk || component->setActive(true) != kResultOk) {
		std::fprintf(stderr, "vst3_programs: not activated\n");
		return 2;
	}
	FUnknownPtr<Vst::IUnitInfo> units(controller);
	int failures = 0;
	if (!units) {
		std::printf("FAIL the controller has no IUnitInfo\n");
		return 1;
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
		if (units->getProgramName(list, i, name) == kResultOk) got.push_back(VST3::StringConvert::convert(name));
	}
	const bool ok = want.empty() ? got.size() <= 1 : got == want;
	std::printf("%s the VST3's preset list (root unit, program list %d): %zu presets%s\n", ok ? "ok  " : "FAIL",
	    static_cast<int>(list), got.size(), want.empty() ? " (none in the component)" : ", the component's factory presets in their order");
	if (!ok)
		for (size_t i = 0; i < got.size() || i < want.size(); i++)
			std::printf("     %zu: %s | %s\n", i, i < got.size() ? got[i].c_str() : "-", i < want.size() ? want[i].c_str() : "-");
	failures += !ok;
	component->setActive(false);
	if (separate) controller->terminate();
	controller = nullptr;
	component->terminate();
	std::printf(failures ? "vst3_programs: %d FAILED\n" : "vst3_programs: all passed\n", failures);
	return failures ? 1 : 0;
}
