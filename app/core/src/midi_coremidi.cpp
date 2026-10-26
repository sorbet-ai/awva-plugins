// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// MIDI in on macOS: CoreMIDI, an input port in the MIDI 2.0 protocol (macOS 11): every source's
// messages arrive as UMP, a MIDI 1.0 source's up-converted by CoreMIDI. Sources come and go with
// the system's setup; refresh() follows them.
#if defined(__APPLE__)
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <map>
#include <mutex>

#include "midi.h"
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

std::string name_of(MIDIEndpointRef e) {
	CFStringRef s = nullptr;
	std::string out;
	if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) == noErr && s) {
		char b[256];
		if (CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8)) out = b;
		CFRelease(s);
	}
	return out.empty() ? "MIDI source" : out;
}

class CoreMidiIn : public MidiIn {
public:
	~CoreMidiIn() override { stop(); }
	void start(Sink sink) override {
		sink_ = std::move(sink);
		if (MIDIClientCreateWithBlock(CFSTR("awva"), &client_, nullptr) != noErr) return;
		Sink* s = &sink_;
		MIDIInputPortCreateWithProtocol(client_, CFSTR("awva in"), kMIDIProtocol_2_0, &port_, ^(const MIDIEventList* list, void* ref) {
			const int slot = static_cast<int>(reinterpret_cast<intptr_t>(ref));
			const MIDIEventPacket* p = &list->packet[0];
			for (UInt32 k = 0; k < list->numPackets; k++) {
				const uint64_t t = p->timeStamp ? host_to_ns(p->timeStamp) : now_ns();
				for (UInt32 i = 0; i < p->wordCount;) {
					const int n = ump_words(p->words[i]);
					if (i + static_cast<UInt32>(n) > p->wordCount) break;
					(*s)(slot, &p->words[i], n, t);
					i += static_cast<UInt32>(n);
				}
				p = MIDIEventPacketNext(p);
			}
		});
		refresh();
	}
	void stop() override {
		if (port_) MIDIPortDispose(port_);
		if (client_) MIDIClientDispose(client_);
		port_ = 0;
		client_ = 0;
		open_.clear();
	}
	void refresh() override {
		if (!port_) return;
		std::map<std::string, MIDIEndpointRef> now;
		const ItemCount n = MIDIGetNumberOfSources();
		for (ItemCount i = 0; i < n; i++) {
			const MIDIEndpointRef e = MIDIGetSource(i);
			std::string name = name_of(e);
			for (int k = 2; now.count(name); k++) name = name_of(e) + " " + std::to_string(k);
			now[name] = e;
		}
		for (auto it = open_.begin(); it != open_.end();) {
			const auto f = now.find(it->first);
			if (f == now.end() || f->second != it->second.e || disabled_.count(it->first)) {
				MIDIPortDisconnectSource(port_, it->second.e);
				it = open_.erase(it);
			} else {
				++it;
			}
		}
		std::vector<MidiPort> listed;
		for (const auto& [name, e] : now) {
			MidiPort p;
			p.name = name;
			p.enabled = !disabled_.count(name);
			if (open_.count(name)) {
				p.state = "open";
			} else if (!p.enabled) {
				p.state = "closed";
			} else {
				int slot = -1;
				for (int s = 0; s < kSlots && slot < 0; s++) {
					bool used = false;
					for (auto& [nm, o] : open_) used = used || o.slot == s;
					if (!used) slot = s;
				}
				if (slot >= 0 && MIDIPortConnectSource(port_, e, reinterpret_cast<void*>(static_cast<intptr_t>(slot))) == noErr) {
					open_[name] = {e, slot};
					p.state = "open";
				} else {
					p.state = "failed";
					p.message = "the source can't be connected";
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
	struct Open {
		MIDIEndpointRef e;
		int slot;
	};
	Sink sink_;
	MIDIClientRef client_ = 0;
	MIDIPortRef port_ = 0;
	std::map<std::string, Open> open_;
	mutable std::mutex m_;
	std::vector<MidiPort> listed_;
};

} // namespace

std::unique_ptr<MidiIn> make_midi_in() { return std::make_unique<CoreMidiIn>(); }

} // namespace awva_app
#endif
