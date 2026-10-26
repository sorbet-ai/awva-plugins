// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// WASAPI, shared mode only: the default render device at the Windows mixer's rate, event driven,
// float samples. The Windows volume and the OS mixer stay in the path (exclusive mode would bypass
// them; the app doesn't offer it). IAudioClient3's smallest shared period that is at least 128
// frames, else the default period. If the device goes away the thread keeps asking for audio in
// real time (as the null sink), so the engine keeps running, and the state says why.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "platform.h"

namespace awva_app {

namespace {

const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const CLSID kEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const IID kIEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const IID kIAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const IID kIAudioClient3 = {0x7ed4ee07, 0x8e67, 0x4cd4, {0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad, 0x42}};
const IID kIRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// the default output changing (headphones plugged in, the user's choice): a flag, read by the UI
// thread, which restarts the audio on the new default
class DefaultWatch final : public IMMNotificationClient {
public:
	explicit DefaultWatch(std::atomic<bool>* flag) : flag_(flag) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
			*out = static_cast<IMMNotificationClient*>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG r = --ref_;
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
		if (flow == eRender && role == eConsole) flag_->store(true);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
	std::atomic<bool>* flag_;
	std::atomic<ULONG> ref_{1};
};

template <class T> void release(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

std::string utf8(const wchar_t* w) {
	if (!w) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

class Wasapi : public AudioOut {
public:
	Wasapi() {
		// (the caller's thread: the UI's, COM ready; notifications come on the system's own thread)
		if (SUCCEEDED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&watch_en_)))) {
			watch_ = new DefaultWatch(&changed_);
			if (FAILED(watch_en_->RegisterEndpointNotificationCallback(watch_))) release(watch_);
		}
	}
	~Wasapi() override {
		stop();
		if (watch_en_ && watch_) watch_en_->UnregisterEndpointNotificationCallback(watch_);
		release(watch_);
		release(watch_en_);
	}
	bool take_default_changed() override { return changed_.exchange(false); }

	int probe_rate() override {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		int rate = 0;
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* dev = nullptr;
		IAudioClient* ac = nullptr;
		WAVEFORMATEX* mix = nullptr;
		if (SUCCEEDED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&en))) &&
		    SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
		    SUCCEEDED(dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) && SUCCEEDED(ac->GetMixFormat(&mix)))
			rate = static_cast<int>(mix->nSamplesPerSec);
		CoTaskMemFree(mix);
		release(ac);
		release(dev);
		release(en);
		return rate;
	}

	bool start(AudioCallback* cb) override {
		stop();
		run_ = true;
		th_ = std::thread([this, cb] { thread(cb); });
		// wait until the device is open (or failed), so info() is meaningful on return
		for (int k = 0; k < 400 && !opened_.load(); k++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return true; // (the thread runs either way: without a device it asks for audio in real time)
	}
	void stop() override {
		run_ = false;
		if (th_.joinable()) th_.join();
		opened_ = false;
	}
	AudioInfo info() const override {
		std::lock_guard<std::mutex> g(m_);
		return info_;
	}

private:
	void set(int state, const std::string& msg) {
		std::lock_guard<std::mutex> g(m_);
		info_.state = state;
		info_.message = msg;
	}
	void thread(AudioCallback* cb) {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD task = 0;
		HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* dev = nullptr;
		IAudioClient* ac = nullptr;
		IAudioClient3* ac3 = nullptr;
		IAudioRenderClient* rc = nullptr;
		WAVEFORMATEX* mix = nullptr;
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		UINT32 frames = 0;
		int channels = 2;
		std::string why;
		{
			std::lock_guard<std::mutex> g(m_);
			info_ = AudioInfo{};
			info_.backend = "WASAPI shared";
		}
		HRESULT hr = CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&en));
		if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
		if (FAILED(hr)) why = "no audio output device";
		if (SUCCEEDED(hr)) {
			IPropertyStore* ps = nullptr;
			if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
				PROPVARIANT v;
				PropVariantInit(&v);
				if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) {
					std::lock_guard<std::mutex> g(m_);
					info_.device = utf8(v.pwszVal);
				}
				PropVariantClear(&v);
				ps->Release();
			}
			hr = dev->Activate(kIAudioClient3, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac3));
			if (SUCCEEDED(hr)) ac = ac3, ac->AddRef();
			else hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
			if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&mix);
			if (FAILED(hr)) why = "the audio device can't be opened";
		}
		WAVEFORMATEXTENSIBLE fmt{};
		if (SUCCEEDED(hr)) {
			channels = std::max<int>(1, mix->nChannels);
			fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			fmt.Format.nChannels = static_cast<WORD>(channels);
			fmt.Format.nSamplesPerSec = mix->nSamplesPerSec;
			fmt.Format.wBitsPerSample = 32;
			fmt.Format.nBlockAlign = static_cast<WORD>(4 * channels);
			fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
			fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			fmt.Samples.wValidBitsPerSample = 32;
			fmt.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask
			                                                               : (channels == 2 ? 3u : 0u);
			fmt.SubFormat = kFloat;
			bool done = false;
			if (ac3) {
				UINT32 def = 0, fund = 0, mn = 0, mx = 0;
				if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(&fmt.Format, &def, &fund, &mn, &mx)) && fund > 0) {
					UINT32 period = mn;
					while (period < 128 && period + fund <= mx) period += fund;
					done = SUCCEEDED(ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, &fmt.Format, nullptr));
				}
			}
			if (!done)
				hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
				    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 0, 0,
				    &fmt.Format, nullptr);
			if (SUCCEEDED(hr)) hr = ac->SetEventHandle(ev);
			if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&frames);
			if (SUCCEEDED(hr)) hr = ac->GetService(kIRenderClient, reinterpret_cast<void**>(&rc));
			if (SUCCEEDED(hr)) {
				BYTE* p = nullptr;
				if (SUCCEEDED(rc->GetBuffer(frames, &p))) rc->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
				hr = ac->Start();
			}
			if (FAILED(hr) && why.empty()) {
				char b[96];
				std::snprintf(b, sizeof b, "the audio device refused shared mode (0x%08lx)", static_cast<unsigned long>(hr));
				why = b;
			}
		}
		const int rate = mix ? static_cast<int>(mix->nSamplesPerSec) : 48000;
		{
			std::lock_guard<std::mutex> g(m_);
			info_.rate = rate;
			info_.buffer = static_cast<int>(frames);
			info_.state = SUCCEEDED(hr) ? AWVA_AUDIO_RUNNING : AWVA_AUDIO_FAILED;
			info_.message = why;
		}
		opened_ = true;
		std::vector<float> l(AudioCallback::kMaxFrames), r(AudioCallback::kMaxFrames);
		bool device = SUCCEEDED(hr);
		auto next = std::chrono::steady_clock::now();
		while (run_) {
			if (device) {
				if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
				UINT32 pad = 0;
				hr = ac->GetCurrentPadding(&pad);
				UINT32 n = SUCCEEDED(hr) ? std::min<UINT32>(frames - pad, AudioCallback::kMaxFrames) : 0;
				BYTE* p = nullptr;
				if (SUCCEEDED(hr) && n > 0) hr = rc->GetBuffer(n, &p);
				if (FAILED(hr)) {
					device = false; // (unplugged, or the format changed: keep running without it)
					set(AWVA_AUDIO_FAILED, hr == AUDCLNT_E_DEVICE_INVALIDATED ? "the audio device went away: Restart audio" : "the audio device stopped");
					next = std::chrono::steady_clock::now();
					continue;
				}
				if (n == 0) continue;
				cb->render(l.data(), r.data(), n, now_ns());
				float* o = reinterpret_cast<float*>(p);
				for (UINT32 i = 0; i < n; i++) {
					float* f = o + static_cast<size_t>(i) * channels;
					f[0] = l[i];
					if (channels > 1) f[1] = r[i];
					for (int c = 2; c < channels; c++) f[c] = 0.0f;
				}
				rc->ReleaseBuffer(n, 0);
			} else {
				constexpr uint32_t n = 256;
				cb->render(l.data(), r.data(), n, now_ns());
				next += std::chrono::nanoseconds(static_cast<int64_t>(1e9 * n / rate));
				std::this_thread::sleep_until(next);
			}
		}
		if (ac) ac->Stop();
		release(rc);
		release(ac3);
		release(ac);
		release(dev);
		release(en);
		CoTaskMemFree(mix);
		CloseHandle(ev);
		if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
		CoUninitialize();
	}

	mutable std::mutex m_;
	AudioInfo info_;
	std::atomic<bool> run_{false}, opened_{false}, changed_{false};
	IMMDeviceEnumerator* watch_en_ = nullptr;
	DefaultWatch* watch_ = nullptr;
	std::thread th_;
};

} // namespace

std::unique_ptr<AudioOut> make_device_audio() { return std::make_unique<Wasapi>(); }

} // namespace awva_app
#endif
