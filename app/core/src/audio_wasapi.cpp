// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// WASAPI on the default render device, event driven.
//   shared (the default): the Windows mixer's rate and format (float); the mixer and the Windows
//     volume stay in the path. IAudioClient3's smallest shared period of at least 128 frames, else
//     the default period.
//   exclusive (on request): the device's own buffer, the size asked for aligned to the device; the
//     first format the device takes (float, 32-, 24- or 16-bit integer) at the mixer's rate, else at
//     48 or 44.1 kHz. Exclusive mode bypasses the mixer and so the Windows volume: the app applies
//     it (IAudioEndpointVolume: the level in dB as a gain, mute; 1 where the device does it in
//     hardware), ramped, updated on the endpoint's notifications. A device whose volume can't be
//     read, or that takes no exclusive format, plays shared, and the state says why.
// If the device goes away the thread keeps asking for audio in real time (as the null sink), so the
// engine keeps running. The latency the driver reports (GetStreamLatency) is in info().
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "endpoint_gain.h"
#include "platform.h"

namespace awva_app {

namespace {

const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const CLSID kEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const IID kIEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const IID kIAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const IID kIAudioClient3 = {0x7ed4ee07, 0x8e67, 0x4cd4, {0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad, 0x42}};
const IID kIRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
const IID kIEndpointVolume = {0x5cdf2c82, 0x841e, 0x4546, {0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a}};
const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

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

// the Windows volume, read from the endpoint (and again on each of its notifications) into a gain
class VolumeWatch final : public IAudioEndpointVolumeCallback {
public:
	// false: the volume can't be read (exclusive mode then isn't safe: shared instead)
	bool open(IMMDevice* dev, EndpointGain* gain) {
		gain_ = gain;
		if (FAILED(dev->Activate(kIEndpointVolume, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&v_))) || !v_) return false;
		if (FAILED(v_->QueryHardwareSupport(&hw_))) hw_ = 0;
		const float g = read();
		if (g < 0) return false;
		gain_->reset(g);
		registered_ = SUCCEEDED(v_->RegisterControlChangeNotify(this));
		return true;
	}
	void close() {
		if (v_ && registered_) v_->UnregisterControlChangeNotify(this);
		registered_ = false;
		release(v_);
	}
	// the gain the mixer would apply now (-1: unreadable)
	float read() {
		float db = 0, scalar = 0;
		BOOL mute = FALSE;
		const bool hw_vol = (hw_ & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0, hw_mute = (hw_ & ENDPOINT_HARDWARE_SUPPORT_MUTE) != 0;
		if (!hw_vol && (FAILED(v_->GetMasterVolumeLevel(&db)) || FAILED(v_->GetMasterVolumeLevelScalar(&scalar)))) return -1;
		if (!hw_mute && FAILED(v_->GetMute(&mute))) return -1;
		return endpoint_gain(db, scalar, mute != FALSE, hw_vol, hw_mute);
	}
	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
		const float g = read();
		gain_->set(g < 0 ? 0.0f : g); // (unreadable now: silent rather than loud)
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
			*out = static_cast<IAudioEndpointVolumeCallback*>(this);
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; } // (owned by the stream, not counted)
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

private:
	IAudioEndpointVolume* v_ = nullptr;
	DWORD hw_ = 0;
	EndpointGain* gain_ = nullptr;
	bool registered_ = false;
};

std::string utf8(const wchar_t* w) {
	if (!w) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

// a format as a WAVEFORMATEXTENSIBLE: float32, or integer of bits in a container of bytes
WAVEFORMATEXTENSIBLE make_format(int rate, int channels, DWORD mask, bool fl, int bits, int container) {
	WAVEFORMATEXTENSIBLE f{};
	f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	f.Format.nChannels = static_cast<WORD>(channels);
	f.Format.nSamplesPerSec = static_cast<DWORD>(rate);
	f.Format.wBitsPerSample = static_cast<WORD>(container * 8);
	f.Format.nBlockAlign = static_cast<WORD>(container * channels);
	f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
	f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	f.Samples.wValidBitsPerSample = static_cast<WORD>(bits);
	f.dwChannelMask = mask;
	f.SubFormat = fl ? kFloat : kPcm;
	return f;
}

// the first exclusive format the device takes: the mixer's rate first, then 48 and 44.1 kHz; float,
// then 32-, 24- (in 32 and in 24) and 16-bit integers; the mixer's channels, then stereo
bool exclusive_format(IAudioClient* ac, const WAVEFORMATEX* mix, WAVEFORMATEXTENSIBLE* out) {
	const DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : 3u;
	const int rates[] = {static_cast<int>(mix->nSamplesPerSec), 48000, 44100};
	struct S {
		bool fl;
		int bits, container;
	};
	const S kinds[] = {{true, 32, 4}, {false, 32, 4}, {false, 24, 4}, {false, 24, 3}, {false, 16, 2}};
	for (const int rate : rates)
		for (const int ch : {static_cast<int>(mix->nChannels), 2})
			for (const S& s : kinds) {
				const WAVEFORMATEXTENSIBLE f = make_format(rate, ch, ch == mix->nChannels ? mask : 3u, s.fl, s.bits, s.container);
				if (ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) == S_OK) {
					*out = f;
					return true;
				}
			}
	return false;
}

// interleave l, r into the device's format (other channels silent), integers rounded and clipped
void write_samples(BYTE* p, const WAVEFORMATEXTENSIBLE& f, const float* l, const float* r, UINT32 n) {
	const int ch = f.Format.nChannels, bytes = f.Format.wBitsPerSample / 8;
	const bool fl = std::memcmp(&f.SubFormat, &kFloat, sizeof(GUID)) == 0 || f.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
	const int valid = f.Samples.wValidBitsPerSample;
	for (UINT32 i = 0; i < n; i++)
		for (int c = 0; c < ch; c++) {
			const float x = c == 0 ? l[i] : (c == 1 ? r[i] : 0.0f);
			BYTE* d = p + (static_cast<size_t>(i) * ch + c) * bytes;
			if (fl) {
				std::memcpy(d, &x, 4);
				continue;
			}
			const int64_t full = int64_t{1} << (valid - 1);
			const int64_t v = std::clamp<int64_t>(std::llround(static_cast<double>(x) * static_cast<double>(full)), -full, full - 1);
			const int64_t s = v << (bytes * 8 - valid); // (left-justified in its container)
			for (int b = 0; b < bytes; b++) d[b] = static_cast<BYTE>((s >> (8 * b)) & 0xff);
		}
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

	// the rate the stream will have: the mixer's (shared), or the exclusive format's
	int probe_rate() override {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		int rate = 0;
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* dev = nullptr;
		IAudioClient* ac = nullptr;
		WAVEFORMATEX* mix = nullptr;
		if (SUCCEEDED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&en))) &&
		    SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
		    SUCCEEDED(dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) && SUCCEEDED(ac->GetMixFormat(&mix))) {
			rate = static_cast<int>(mix->nSamplesPerSec);
			WAVEFORMATEXTENSIBLE f;
			if (config_.exclusive && exclusive_format(ac, mix, &f)) rate = static_cast<int>(f.Format.nSamplesPerSec);
		}
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
		AudioInfo i = info_;
		if (i.mode == "exclusive") i.volume = gain_.applied();
		return i;
	}

private:
	void set(int state, const std::string& msg) {
		std::lock_guard<std::mutex> g(m_);
		info_.state = state;
		info_.message = msg;
	}

	// shared: the mixer's format; IAudioClient3's small period where it can
	HRESULT open_shared(IMMDevice* dev, IAudioClient** ac, WAVEFORMATEXTENSIBLE* fmt, const WAVEFORMATEX* mix, HANDLE ev, UINT32* period) {
		IAudioClient3* ac3 = nullptr;
		HRESULT hr = dev->Activate(kIAudioClient3, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac3));
		if (SUCCEEDED(hr)) {
			*ac = ac3;
		} else {
			hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ac));
			if (FAILED(hr)) return hr;
		}
		const int channels = std::max<int>(1, mix->nChannels);
		*fmt = make_format(static_cast<int>(mix->nSamplesPerSec), channels,
		    mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : (channels == 2 ? 3u : 0u),
		    true, 32, 4);
		bool done = false;
		*period = 0;
		if (ac3) {
			UINT32 def = 0, fund = 0, mn = 0, mx = 0;
			if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(&fmt->Format, &def, &fund, &mn, &mx)) && fund > 0) {
				UINT32 p = mn;
				while (p < 128 && p + fund <= mx) p += fund;
				done = SUCCEEDED(ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, &fmt->Format, nullptr));
				if (done) *period = p;
			}
		}
		if (!done)
			hr = (*ac)->Initialize(AUDCLNT_SHAREMODE_SHARED,
			    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 0, 0, &fmt->Format,
			    nullptr);
		if (SUCCEEDED(hr)) hr = (*ac)->SetEventHandle(ev);
		return hr;
	}

	// exclusive: the device's format, the buffer asked for aligned to the device
	HRESULT open_exclusive(IMMDevice* dev, IAudioClient** ac, WAVEFORMATEXTENSIBLE* fmt, const WAVEFORMATEX* mix, HANDLE ev, std::string* why) {
		HRESULT hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ac));
		if (FAILED(hr)) return hr;
		if (!exclusive_format(*ac, mix, fmt)) {
			*why = "the device takes no exclusive format: playing shared";
			return E_FAIL;
		}
		REFERENCE_TIME def = 0, mn = 0;
		(*ac)->GetDevicePeriod(&def, &mn);
		const double rate = fmt->Format.nSamplesPerSec;
		// (frames 0: the device's smallest buffer)
		REFERENCE_TIME want = config_.frames > 0 ? static_cast<REFERENCE_TIME>(std::llround(config_.frames * 1e7 / rate)) : mn;
		want = std::max(want, mn);
		hr = (*ac)->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, want, &fmt->Format, nullptr);
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) { // (the device's size, from a new client)
			UINT32 n = 0;
			(*ac)->GetBufferSize(&n);
			release(*ac);
			want = static_cast<REFERENCE_TIME>(std::llround(n * 1e7 / rate));
			hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ac));
			if (SUCCEEDED(hr)) hr = (*ac)->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, want, &fmt->Format, nullptr);
		}
		if (SUCCEEDED(hr)) hr = (*ac)->SetEventHandle(ev);
		if (FAILED(hr)) {
			char b[120];
			std::snprintf(b, sizeof b, "exclusive mode refused (0x%08lx; another app may hold the device): playing shared", static_cast<unsigned long>(hr));
			*why = b;
		}
		return hr;
	}

	void thread(AudioCallback* cb) {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD task = 0;
		HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* dev = nullptr;
		IAudioClient* ac = nullptr;
		IAudioRenderClient* rc = nullptr;
		WAVEFORMATEX* mix = nullptr;
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		UINT32 frames = 0, period = 0;
		WAVEFORMATEXTENSIBLE fmt{};
		std::string why, note;
		bool exclusive = false;
		VolumeWatch vol;
		{
			std::lock_guard<std::mutex> g(m_);
			info_ = AudioInfo{};
			info_.backend = "WASAPI";
			info_.exclusive_available = true;
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
			IAudioClient* probe = nullptr;
			hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&probe));
			if (SUCCEEDED(hr)) hr = probe->GetMixFormat(&mix);
			release(probe);
			if (FAILED(hr)) why = "the audio device can't be opened";
		}
		if (SUCCEEDED(hr) && config_.exclusive) {
			// exclusive only with the Windows volume applied by the app
			if (!vol.open(dev, &gain_)) {
				note = "the Windows volume can't be read: playing shared";
			} else {
				exclusive = SUCCEEDED(open_exclusive(dev, &ac, &fmt, mix, ev, &note));
				if (!exclusive) {
					vol.close();
					release(ac);
				}
			}
		}
		if (SUCCEEDED(hr) && !exclusive) hr = open_shared(dev, &ac, &fmt, mix, ev, &period);
		if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&frames);
		if (SUCCEEDED(hr)) hr = ac->GetService(kIRenderClient, reinterpret_cast<void**>(&rc));
		if (SUCCEEDED(hr)) {
			BYTE* p = nullptr;
			if (SUCCEEDED(rc->GetBuffer(frames, &p))) rc->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
			hr = ac->Start();
		}
		if (FAILED(hr) && why.empty()) {
			char b[96];
			std::snprintf(b, sizeof b, "the audio device refused the stream (0x%08lx)", static_cast<unsigned long>(hr));
			why = b;
		}
		REFERENCE_TIME lat = 0;
		const bool have_lat = SUCCEEDED(hr) && SUCCEEDED(ac->GetStreamLatency(&lat));
		const int rate = SUCCEEDED(hr) ? static_cast<int>(fmt.Format.nSamplesPerSec) : (mix ? static_cast<int>(mix->nSamplesPerSec) : 48000);
		if (exclusive) period = frames;
		if (!exclusive && period == 0 && SUCCEEDED(hr)) { // (no IAudioClient3 period: the engine's own)
			REFERENCE_TIME def = 0, mn = 0;
			if (SUCCEEDED(ac->GetDevicePeriod(&def, &mn))) period = static_cast<UINT32>(std::llround(def / 1e7 * fmt.Format.nSamplesPerSec));
		}
		// shared: one period queued ahead of the mixer, not the whole buffer (the least latency the
		// device's period allows; the event comes each time the mixer took a period)
		const UINT32 target = exclusive ? frames : std::clamp<UINT32>(period ? period : frames, 1, frames);
		{
			std::lock_guard<std::mutex> g(m_);
			info_.mode = exclusive ? "exclusive" : "shared";
			info_.rate = rate;
			info_.buffer = static_cast<int>(frames);
			info_.period = static_cast<int>(period);
			info_.stream_ms = have_lat ? static_cast<float>(lat / 1e4) : -1.0f;
			info_.state = SUCCEEDED(hr) ? AWVA_AUDIO_RUNNING : AWVA_AUDIO_FAILED;
			info_.message = SUCCEEDED(hr) ? note : why;
		}
		opened_ = true;
		std::vector<float> l(AudioCallback::kMaxFrames), r(AudioCallback::kMaxFrames);
		bool device = SUCCEEDED(hr);
		const int channels = fmt.Format.nChannels;
		auto next = std::chrono::steady_clock::now();
		while (run_) {
			if (device) {
				if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
				UINT32 n = frames;
				if (!exclusive) { // (shared: up to one period queued)
					UINT32 pad = 0;
					hr = ac->GetCurrentPadding(&pad);
					n = SUCCEEDED(hr) && pad < target ? target - pad : 0;
				}
				n = std::min<UINT32>(n, AudioCallback::kMaxFrames);
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
				if (exclusive) gain_.apply(l.data(), r.data(), n); // (the Windows volume, as the mixer would)
				cb->tap(l.data(), r.data(), n);
				if (channels == 2 && fmt.Format.wBitsPerSample == 32 && std::memcmp(&fmt.SubFormat, &kFloat, sizeof(GUID)) == 0) {
					float* o = reinterpret_cast<float*>(p);
					for (UINT32 i = 0; i < n; i++) o[2 * i] = l[i], o[2 * i + 1] = r[i];
				} else {
					write_samples(p, fmt, l.data(), r.data(), n);
				}
				rc->ReleaseBuffer(n, 0);
			} else {
				constexpr uint32_t n = 256;
				cb->render(l.data(), r.data(), n, now_ns());
				cb->tap(l.data(), r.data(), n);
				next += std::chrono::nanoseconds(static_cast<int64_t>(1e9 * n / rate));
				std::this_thread::sleep_until(next);
			}
		}
		if (ac) ac->Stop();
		vol.close();
		release(rc);
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
	EndpointGain gain_;
	std::atomic<bool> run_{false}, opened_{false}, changed_{false};
	std::thread th_;
	IMMDeviceEnumerator* watch_en_ = nullptr;
	DefaultWatch* watch_ = nullptr;
};

} // namespace

std::unique_ptr<AudioOut> make_device_audio() { return std::make_unique<Wasapi>(); }

} // namespace awva_app
#endif
