// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// MIDI in from every port the OS has, as UMP:
//   Windows: WinMM (through Windows MIDI Services where it runs, which makes the ports
//            multi-client): MIDI 1.0 messages as MIDI 1.0 in UMP (type 2), which the component
//            up-converts (MPE included) exactly as the plugins' MIDI dialect does
//   macOS:   CoreMIDI in the MIDI 2.0 protocol: UMP as CoreMIDI gives it (a MIDI 1.0 source
//            up-converted by the OS)
// Every port is opened unless the user turned it off. A port another app holds (WinMM without
// Windows MIDI Services is single-client) is listed as busy, with what to do.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace awva_app {

struct MidiPort {
	std::string name;
	bool enabled = true;
	std::string state; // "open", "closed", "busy", "failed"
	std::string message;
};

class MidiIn {
public:
	static constexpr int kSlots = 16;
	// one UMP packet from slot s (each slot's calls come from one thread at a time)
	using Sink = std::function<void(int slot, const uint32_t* words, int count, uint64_t ns)>;
	virtual ~MidiIn() = default;
	virtual void start(Sink sink) = 0;
	virtual void stop() = 0;
	// the OS's ports again: new ones opened (unless turned off), gone ones closed
	virtual void refresh() = 0;
	virtual std::vector<MidiPort> ports() const = 0;
	void set_disabled(const std::set<std::string>& names) { disabled_ = names; }
	const std::set<std::string>& disabled() const { return disabled_; }

protected:
	std::set<std::string> disabled_;
};

std::unique_ptr<MidiIn> make_midi_in();

// a UMP packet's length in words, by its message type
inline int ump_words(uint32_t w0) {
	static const int n[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
	return n[w0 >> 28];
}

} // namespace awva_app
