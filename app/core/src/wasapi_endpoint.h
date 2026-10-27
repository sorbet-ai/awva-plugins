// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A WASAPI endpoint's identity, read from its properties and its topology (nothing opened, nothing
// played): its id, its devnode's enumerator (PKEY_Device_EnumeratorName), the device interface path
// of the kernel streaming filter it's connected to, and the bus they name (buffer_policy.h).
#pragma once
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <devicetopology.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <string>

#include "buffer_policy.h"

namespace awva_app {

inline std::string wide_to_utf8(const wchar_t* w) {
	if (!w) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

inline std::wstring utf8_to_wide(const std::string& s) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

struct EndpointId {
	std::string id, enumerator, connected;
	Bus bus = Bus::Other;
};

inline EndpointId endpoint_id(IMMDevice* dev) {
	EndpointId e;
	LPWSTR w = nullptr;
	if (SUCCEEDED(dev->GetId(&w))) e.id = wide_to_utf8(w);
	CoTaskMemFree(w);
	IPropertyStore* ps = nullptr;
	if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
		static const PROPERTYKEY kEnumeratorName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(ps->GetValue(kEnumeratorName, &v)) && v.vt == VT_LPWSTR) e.enumerator = wide_to_utf8(v.pwszVal);
		PropVariantClear(&v);
		ps->Release();
	}
	IDeviceTopology* top = nullptr;
	if (SUCCEEDED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&top))) && top) {
		IConnector* c = nullptr;
		if (SUCCEEDED(top->GetConnector(0, &c)) && c) {
			LPWSTR to = nullptr;
			if (SUCCEEDED(c->GetDeviceIdConnectedTo(&to))) e.connected = wide_to_utf8(to);
			CoTaskMemFree(to);
			c->Release();
		}
		top->Release();
	}
	e.bus = bus_of(e.enumerator, e.connected);
	return e;
}

} // namespace awva_app
#endif
