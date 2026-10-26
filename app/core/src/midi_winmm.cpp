// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// MIDI in on Windows through WinMM. Where Windows MIDI Services runs (Windows 11, midisrv), WinMM's
// ports are its endpoints and several apps can open one; without it a USB port is single-client,
// and a port another app holds fails to open with MMSYSERR_ALLOCATED: listed as busy.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <map>
#include <mutex>

#include "midi.h"
#include "platform.h"

namespace awva_app {

namespace {

std::string utf8(const wchar_t* w) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

class WinMidi : public MidiIn {
public:
	~WinMidi() override { stop(); }
	void start(Sink sink) override {
		sink_ = std::move(sink);
		refresh();
	}
	void stop() override {
		for (auto& [name, p] : open_) close(p);
		open_.clear();
		listed_.clear();
	}
	void refresh() override {
		// the ports now, by name (a second port of the same name gets " 2")
		std::map<std::string, UINT> now;
		const UINT n = midiInGetNumDevs();
		for (UINT i = 0; i < n; i++) {
			MIDIINCAPSW caps;
			if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
			std::string name = utf8(caps.szPname);
			for (int k = 2; now.count(name); k++) name = utf8(caps.szPname) + " " + std::to_string(k);
			now[name] = i;
		}
		// close the ones gone or turned off
		for (auto it = open_.begin(); it != open_.end();) {
			if (!now.count(it->first) || disabled_.count(it->first)) {
				close(it->second);
				it = open_.erase(it);
			} else {
				++it;
			}
		}
		std::vector<MidiPort> listed;
		for (const auto& [name, id] : now) {
			MidiPort p;
			p.name = name;
			p.enabled = !disabled_.count(name);
			if (open_.count(name)) {
				p.state = "open";
			} else if (!p.enabled) {
				p.state = "closed";
			} else {
				const MMRESULT r = open(name, id);
				if (r == MMSYSERR_NOERROR) {
					p.state = "open";
				} else if (r == MMSYSERR_ALLOCATED) {
					p.state = "busy";
					p.message = "another app has this port open; close it there (or install Windows MIDI Services, which lets apps share ports)";
				} else {
					p.state = "failed";
					p.message = "the port can't be opened (" + std::to_string(r) + ")";
				}
			}
			listed.push_back(p);
		}
		std::lock_guard<std::mutex> g(m_);
		listed_ = listed;
	}
	std::vector<MidiPort> ports() const override {
		std::lock_guard<std::mutex> g(m_);
		return listed_;
	}

private:
	struct Port {
		HMIDIIN h = nullptr;
		int slot = -1;
		WinMidi* self = nullptr;
	};
	MMRESULT open(const std::string& name, UINT id) {
		int slot = -1;
		for (int s = 0; s < kSlots && slot < 0; s++) {
			bool used = false;
			for (auto& [n, p] : open_) used = used || p->slot == s;
			if (!used) slot = s;
		}
		if (slot < 0) return MMSYSERR_NOMEM;
		auto p = std::make_unique<Port>();
		p->slot = slot;
		p->self = this;
		MMRESULT r = midiInOpen(&p->h, id, reinterpret_cast<DWORD_PTR>(&WinMidi::proc), reinterpret_cast<DWORD_PTR>(p.get()), CALLBACK_FUNCTION);
		if (r != MMSYSERR_NOERROR) return r;
		r = midiInStart(p->h);
		if (r != MMSYSERR_NOERROR) {
			midiInClose(p->h);
			return r;
		}
		open_[name] = std::move(p);
		return MMSYSERR_NOERROR;
	}
	static void close(std::unique_ptr<Port>& p) {
		if (!p || !p->h) return;
		midiInStop(p->h);
		midiInReset(p->h);
		midiInClose(p->h);
		p->h = nullptr;
	}
	static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
		if (msg != MIM_DATA) return;
		const uint64_t t = now_ns();
		auto* p = reinterpret_cast<Port*>(inst);
		const uint32_t m = static_cast<uint32_t>(p1);
		const uint32_t status = m & 0xff;
		if (status < 0x80 || status >= 0xf0) return; // (system messages: not notes)
		const uint32_t hi = status & 0xf0;
		const bool two = hi == 0xc0 || hi == 0xd0;
		const uint32_t w = 0x20000000u | status << 16 | ((m >> 8) & 0x7f) << 8 | (two ? 0u : (m >> 16) & 0x7f);
		p->self->sink_(p->slot, &w, 1, t);
	}

	Sink sink_;
	std::map<std::string, std::unique_ptr<Port>> open_;
	mutable std::mutex m_;
	std::vector<MidiPort> listed_;
};

} // namespace

std::unique_ptr<MidiIn> make_midi_in() { return std::make_unique<WinMidi>(); }

} // namespace awva_app
#endif
