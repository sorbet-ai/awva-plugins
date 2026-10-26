// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// What the default output device offers, asked without opening a stream (no Initialize, no Start:
// nothing plays, the device isn't taken): its mixer format, the shared periods IAudioClient3 offers,
// the exclusive periods, and whether it applies its volume in hardware. For the latency figures and
// awva_app_cli devices.
#include <string>

#include "json.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#endif

namespace awva_app {

std::string query_output_json() {
#if defined(_WIN32)
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	std::string s = "{";
	IMMDeviceEnumerator* en = nullptr;
	IMMDevice* dev = nullptr;
	IAudioClient* ac = nullptr;
	IAudioClient3* ac3 = nullptr;
	IAudioEndpointVolume* vol = nullptr;
	WAVEFORMATEX* mix = nullptr;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) &&
	    SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
	    SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) && SUCCEEDED(ac->GetMixFormat(&mix))) {
		const double rate = mix->nSamplesPerSec;
		s += "\"rate\":" + std::to_string(mix->nSamplesPerSec) + ",\"channels\":" + std::to_string(mix->nChannels) + ",\"bits\":" + std::to_string(mix->wBitsPerSample);
		REFERENCE_TIME def = 0, mn = 0;
		if (SUCCEEDED(ac->GetDevicePeriod(&def, &mn)))
			s += ",\"exclusive_default_ms\":" + jnum(def / 1e4) + ",\"exclusive_min_ms\":" + jnum(mn / 1e4) + ",\"exclusive_min_frames\":" + jnum(mn / 1e7 * rate);
		IAudioClient3* c3 = nullptr;
		if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&c3)))) {
			ac3 = c3;
			UINT32 d = 0, f = 0, lo = 0, hi = 0;
			if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(mix, &d, &f, &lo, &hi)))
				s += ",\"shared_default_frames\":" + std::to_string(d) + ",\"shared_fundamental_frames\":" + std::to_string(f) + ",\"shared_min_frames\":" +
				    std::to_string(lo) + ",\"shared_max_frames\":" + std::to_string(hi) + ",\"shared_min_ms\":" + jnum(lo * 1e3 / rate);
			else
				s += ",\"shared_low_latency\":false";
		}
		if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&vol)))) {
			DWORD hw = 0;
			vol->QueryHardwareSupport(&hw);
			s += std::string(",\"hardware_volume\":") + ((hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? "true" : "false");
		}
	} else {
		s += "\"error\":\"no default output\"";
	}
	CoTaskMemFree(mix);
	if (vol) vol->Release();
	if (ac3) ac3->Release();
	if (ac) ac->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	return s + "}";
#else
	return "{}";
#endif
}

} // namespace awva_app
