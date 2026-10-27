// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// WASAPI on the default render device, or the one chosen (its endpoint id; one that isn't there, or
// isn't active, plays the default and says so), event driven.
//   shared (the default): the Windows mixer's rate and format (float); the mixer and the Windows
//     volume stay in the path. IAudioClient3's smallest shared period of at least 128 frames, else
//     the default period.
//   exclusive (on request): the device's own buffer, the size asked for aligned to the device; the
//     first format the device takes (float, 32-, 24- or 16-bit integer) at the mixer's rate, else at
//     48 or 44.1 kHz. Exclusive mode bypasses the mixer and so the Windows volume: the app applies
//     it (IAudioEndpointVolume: the level in dB as a gain, mute; 1 where the device does it in
//     hardware), ramped, updated on the endpoint's notifications. A device whose volume can't be
//     read, or that takes no exclusive format, plays shared, and the state says why.
// A Bluetooth endpoint (its bus, from its devnode and topology: wasapi_endpoint.h) gets no exclusive
// buffer it can't sustain: buffer_policy.h raises it on its glitches, or plays shared and says so.
// If the device goes away the thread keeps asking for audio in real time (as the null sink), so the
// engine keeps running. The latency the driver reports (GetStreamLatency) is in info().
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
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
#include "buffer_policy.h"
#include "endpoint_gain.h"
#include "platform.h"
#include "wasapi_endpoint.h"

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

// the default output changing (headphones plugged in, the user's choice), or an output coming or
// going: flags, read by the UI thread, which restarts the audio on the new default or the chosen one
class DefaultWatch final : public IMMNotificationClient {
public:
	DefaultWatch(std::atomic<bool>* flag, std::atomic<bool>* reopen, std::atomic<bool>* devices) : flag_(flag), reopen_(reopen), devices_(devices) {}
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
	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
		devices_->store(true);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
		devices_->store(true);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
		devices_->store(true);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
		// PKEY_AudioEngine_DeviceFormat: a device's format changed (the stream is opened again on it)
		static const GUID kFormat = {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}};
		if (key.fmtid == kFormat && key.pid == 0) reopen_->store(true);
		return S_OK;
	}

private:
	std::atomic<bool>* flag_;
	std::atomic<bool>* reopen_;
	std::atomic<bool>* devices_;
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

// the endpoint's friendly name ("" unreadable)
std::string friendly_name(IMMDevice* dev) {
	std::string n;
	IPropertyStore* ps = nullptr;
	if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) n = wide_to_utf8(v.pwszVal);
		PropVariantClear(&v);
		ps->Release();
	}
	return n;
}

// the output to open: the chosen one (an active endpoint by its id), else the default (missing: the
// chosen one isn't there); nullptr: no output at all
IMMDevice* find_output(IMMDeviceEnumerator* en, const std::string& id, bool* missing) {
	IMMDevice* dev = nullptr;
	*missing = false;
	if (!id.empty()) {
		DWORD state = 0;
		if (SUCCEEDED(en->GetDevice(utf8_to_wide(id).c_str(), &dev)) && dev && SUCCEEDED(dev->GetState(&state)) && state == DEVICE_STATE_ACTIVE) return dev;
		release(dev);
		*missing = true;
	}
	if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) dev = nullptr;
	return dev;
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
			watch_ = new DefaultWatch(&changed_, &reopen_, &devices_changed_);
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
	bool take_devices_changed() override { return devices_changed_.exchange(false); }

	std::vector<OutputDevice> devices() override {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		std::vector<OutputDevice> v;
		IMMDeviceEnumerator* en = nullptr;
		IMMDeviceCollection* all = nullptr;
		IMMDevice* def = nullptr;
		if (FAILED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&en)))) return v;
		std::string def_id;
		if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) def_id = endpoint_id(def).id;
		release(def);
		UINT n = 0;
		if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) && SUCCEEDED(all->GetCount(&n)))
			for (UINT k = 0; k < n; k++) {
				IMMDevice* dev = nullptr;
				if (FAILED(all->Item(k, &dev)) || !dev) continue;
				const EndpointId id = endpoint_id(dev);
				v.push_back({id.id, friendly_name(dev), bus_name(id.bus), id.id == def_id});
				release(dev);
			}
		release(all);
		release(en);
		return v;
	}

	// the rate the stream will have: the mixer's (shared), or the exclusive format's
	int probe_rate() override {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		int rate = 0;
		IMMDeviceEnumerator* en = nullptr;
		IMMDevice* dev = nullptr;
		IAudioClient* ac = nullptr;
		WAVEFORMATEX* mix = nullptr;
		bool missing = false;
		if (SUCCEEDED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&en))) &&
		    (dev = find_output(en, config_.device, &missing)) != nullptr &&
		    SUCCEEDED(dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) && SUCCEEDED(ac->GetMixFormat(&mix))) {
			rate = static_cast<int>(mix->nSamplesPerSec);
			WAVEFORMATEXTENSIBLE f;
			const auto learned = config_.learned.find(endpoint_id(dev).id);
			const bool shared_only = learned != config_.learned.end() && learned->second < 0;
			if (config_.exclusive && !shared_only && exclusive_format(ac, mix, &f)) rate = static_cast<int>(f.Format.nSamplesPerSec);
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
		policy_ = EndpointBuffer{}; // (what was learned comes from config_.learned again)
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
		i.glitches = glitches_.load();
		i.recoveries = recoveries_.load();
		return i;
	}
	int take_glitches(Glitch* out, int max) override {
		int k = 0;
		for (size_t r = gr_.load(std::memory_order_relaxed); k < max && r != gw_.load(std::memory_order_acquire); r++) {
			out[k++] = gring_[r % kGlitchRing];
			gr_.store(r + 1, std::memory_order_release);
		}
		return k;
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
		// (frames 0: the device's smallest buffer; a Bluetooth endpoint's: what it sustains)
		REFERENCE_TIME want = config_.frames > 0 ? static_cast<REFERENCE_TIME>(std::llround(config_.frames * 1e7 / rate)) : mn;
		const int f = policy_.frames(config_.frames, static_cast<int>(std::llround(mn * rate / 1e7)), static_cast<int>(std::llround(def * rate / 1e7)));
		if (policy_.bus() == Bus::Bluetooth) want = static_cast<REFERENCE_TIME>(std::llround(f * 1e7 / rate));
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

	// the audio thread: the stream opened, played, and opened again when it keeps glitching, when the
	// device fails (AUDCLNT_E_*) or its format changes; without a device it asks for audio in real time
	void thread(AudioCallback* cb) {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD task = 0;
		HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
		if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
		std::vector<float> l(AudioCallback::kMaxFrames), r(AudioCallback::kMaxFrames);
		bool first = true;
		while (run_) {
			const Outcome o = run_stream(cb, l.data(), r.data(), first);
			first = false;
			if (o == Outcome::Stopped) break;
			if (o == Outcome::Reopen) {
				recoveries_.fetch_add(1);
				continue;
			}
			// (no device: in real time without one, until stopped; Restart audio or a new default
			// output opens one again)
			const int rate = info().rate > 0 ? info().rate : 48000;
			auto next = std::chrono::steady_clock::now();
			while (run_) {
				constexpr uint32_t n = 256;
				cb->render(l.data(), r.data(), n, now_ns());
				cb->tap(l.data(), r.data(), n);
				next += std::chrono::nanoseconds(static_cast<int64_t>(1e9 * n / rate));
				std::this_thread::sleep_until(next);
			}
		}
		if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
		CoUninitialize();
	}

	enum class Outcome { Stopped, Reopen, Failed };

	// a glitch: noted (a lock-free ring the UI thread logs from) and counted; true when they keep
	// coming (more than 3 in a second): the stream is opened again
	bool glitch(int kind, uint64_t t, int64_t dt_us, int64_t render_us, uint32_t pad) {
		glitches_.fetch_add(1);
		const size_t w = gw_.load(std::memory_order_relaxed);
		if (w - gr_.load(std::memory_order_acquire) < kGlitchRing) {
			gring_[w % kGlitchRing] = {t, kind, dt_us, render_us, pad};
			gw_.store(w + 1, std::memory_order_release);
		}
		recent_[recent_n_++ % 4] = t;
		const bool storm = recent_n_ >= 4 && t - recent_[recent_n_ % 4] < 1000000000ull;
		if (storm && t - last_reopen_ > 3000000000ull) {
			last_reopen_ = t;
			return true;
		}
		return false;
	}

	// a glitch of the endpoint in an exclusive stream of frames: true when the policy opens the stream
	// again (a Bluetooth endpoint's buffer raised, or shared)
	bool step_up(uint64_t t, UINT32 frames) {
		if (policy_.glitch(t, static_cast<int>(frames)) == EndpointBuffer::Action::None) return false;
		std::lock_guard<std::mutex> g(m_);
		info_.learned = policy_.learned();
		return true;
	}

	Outcome run_stream(AudioCallback* cb, float* l, float* r, bool first) {
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
			const std::string device = info_.device;
			info_ = AudioInfo{};
			info_.backend = "WASAPI";
			info_.exclusive_available = true;
			info_.device = device;
		}
		bool missing = false;
		HRESULT hr = CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kIEnumerator, reinterpret_cast<void**>(&en));
		if (SUCCEEDED(hr)) {
			dev = find_output(en, config_.device, &missing);
			if (!dev) hr = E_FAIL;
		}
		{
			std::lock_guard<std::mutex> g(m_);
			info_.missing = missing;
		}
		if (FAILED(hr)) why = "no audio output device";
		if (SUCCEEDED(hr)) {
			const std::string name = friendly_name(dev);
			if (!name.empty()) {
				std::lock_guard<std::mutex> g(m_);
				info_.device = name;
			}
			const EndpointId id = endpoint_id(dev);
			const auto learned = config_.learned.find(id.id);
			policy_.endpoint(id.id, id.bus, learned == config_.learned.end() ? 0 : learned->second);
			{
				std::lock_guard<std::mutex> g(m_);
				info_.endpoint = id.id;
				info_.bus = bus_name(id.bus);
			}
			IAudioClient* probe = nullptr;
			hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&probe));
			if (SUCCEEDED(hr)) hr = probe->GetMixFormat(&mix);
			release(probe);
			if (FAILED(hr)) why = "the audio device can't be opened";
		}
		if (SUCCEEDED(hr) && config_.exclusive && !policy_.shared_only()) {
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
		if (SUCCEEDED(hr) && note.empty()) note = policy_.note(config_.exclusive);
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
			info_.learned = policy_.learned();
			info_.state = SUCCEEDED(hr) ? AWVA_AUDIO_RUNNING : AWVA_AUDIO_FAILED;
			info_.message = SUCCEEDED(hr) ? note : why;
		}
		if (first) opened_ = true;
		Outcome out = SUCCEEDED(hr) ? Outcome::Stopped : Outcome::Failed;
		const int channels = fmt.Format.nChannels;
		const bool fast_copy = channels == 2 && fmt.Format.wBitsPerSample == 32 && std::memcmp(&fmt.SubFormat, &kFloat, sizeof(GUID)) == 0;
		const uint64_t period_ns = period ? static_cast<uint64_t>(1e9 * period / rate) : 10000000ull;
		uint64_t last = 0;
		reopen_.store(false);
		while (SUCCEEDED(hr) && run_) {
			if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) {
				// (no event for 200 ms: the stream stalled)
				const uint64_t at = now_ns();
				const bool storm = glitch(3, at, 200000, 0, 0);
				const bool stepped = exclusive && step_up(at, frames); // (every glitch counted by the policy)
				if (storm || stepped || reopen_.exchange(false)) {
					out = Outcome::Reopen;
					break;
				}
				continue;
			}
			const uint64_t t = now_ns();
			// late: the event more than a period and a half after the last (the device starved)
			if (last && t - last > period_ns + period_ns / 2 + 1000000ull) {
				const bool storm = glitch(1, t, static_cast<int64_t>((t - last) / 1000), 0, 0);
				const bool stepped = exclusive && step_up(t, frames);
				if (storm || stepped) {
					out = Outcome::Reopen;
					break;
				}
			}
			last = t;
			if (reopen_.exchange(false)) { // (the device's format changed)
				out = Outcome::Reopen;
				break;
			}
			UINT32 n = frames, pad = 0;
			if (!exclusive) { // (shared: up to one period queued)
				hr = ac->GetCurrentPadding(&pad);
				n = SUCCEEDED(hr) && pad < target ? target - pad : 0;
			}
			n = std::min<UINT32>(n, AudioCallback::kMaxFrames);
			BYTE* p = nullptr;
			if (SUCCEEDED(hr) && n > 0) hr = rc->GetBuffer(n, &p);
			if (FAILED(hr)) {
				// (unplugged, or the device changed: open it again, else without it)
				glitch(4, t, 0, 0, 0);
				set(AWVA_AUDIO_FAILED, hr == AUDCLNT_E_DEVICE_INVALIDATED ? "the audio device went away" : "the audio device stopped: opening it again");
				out = hr == AUDCLNT_E_DEVICE_INVALIDATED ? Outcome::Failed : Outcome::Reopen;
				break;
			}
			if (n == 0) continue;
			cb->render(l, r, n, t);
			if (exclusive) gain_.apply(l, r, n); // (the Windows volume, as the mixer would)
			cb->tap(l, r, n);
			if (fast_copy) {
				float* o = reinterpret_cast<float*>(p);
				for (UINT32 i = 0; i < n; i++) o[2 * i] = l[i], o[2 * i + 1] = r[i];
			} else {
				write_samples(p, fmt, l, r, n);
			}
			rc->ReleaseBuffer(n, 0);
			// slow: the render took most of the period (the stream is about to starve)
			const uint64_t took = now_ns() - t;
			if (took > period_ns - period_ns / 8 && glitch(2, t, 0, static_cast<int64_t>(took / 1000), pad)) {
				out = Outcome::Reopen;
				break;
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
		if (out == Outcome::Reopen) {
			std::lock_guard<std::mutex> g(m_);
			info_.message = "audio re-opened after glitches";
		}
		return out;
	}

	mutable std::mutex m_;
	AudioInfo info_;
	EndpointGain gain_;
	EndpointBuffer policy_; // (the audio thread's, between start and stop)
	std::atomic<bool> run_{false}, opened_{false}, changed_{false}, reopen_{false}, devices_changed_{false};
	// the glitches: counted, and noted in a ring the UI thread logs from (audio thread: writer)
	static constexpr size_t kGlitchRing = 64;
	Glitch gring_[kGlitchRing];
	std::atomic<size_t> gw_{0}, gr_{0};
	std::atomic<uint32_t> glitches_{0}, recoveries_{0};
	uint64_t recent_[4] = {}, last_reopen_ = 0;
	uint32_t recent_n_ = 0;
	std::thread th_;
	IMMDeviceEnumerator* watch_en_ = nullptr;
	DefaultWatch* watch_ = nullptr;
};

} // namespace

std::unique_ptr<AudioOut> make_device_audio() { return std::make_unique<Wasapi>(); }

} // namespace awva_app
#endif
