// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The exclusive buffer an output endpoint sustains (any backend, and the null sink's fake endpoint).
//   A wired or USB endpoint plays the buffer asked for, aligned to the device (and a glitch storm
//   opens the stream again, as the backend does for every endpoint).
//   A Bluetooth endpoint delivers its wake-ups in the radio's bursts (about 20 ms apart), which a
//   small exclusive buffer can't ride. Its buffer starts at the device's default period at least;
//   kGlitches glitches of the endpoint (late wake-ups, no events) within kWindowNs double it; past
//   twice the default period (shared mode's own buffering: a period queued by the app and one in the
//   mixer) shared mode plays as soon and through the mixer, so it plays shared, and says so. What it
//   learned is kept per endpoint (the app's settings), and asking for exclusive mode again forgets it.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace awva_app {

enum class Bus { Other, Usb, Bluetooth };

inline const char* bus_name(Bus b) { return b == Bus::Bluetooth ? "bluetooth" : (b == Bus::Usb ? "usb" : "other"); }

inline Bus bus_from_name(const std::string& s) { return s == "bluetooth" ? Bus::Bluetooth : (s == "usb" ? Bus::Usb : Bus::Other); }

// the bus from the endpoint's devnode enumerator ("BTHENUM", "USB", "HDAUDIO", ...) and the device
// interface path of the kernel streaming filter it's connected to ("\\?\bthenum#...",
// "\\?\bthledevice#...", "\\?\usb#..."): the bus drivers' names, any case
inline Bus bus_of(const std::string& enumerator, const std::string& path) {
	std::string s = enumerator + "|" + path;
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	// classic (A2DP: BTHENUM; hands-free: BTHHFENUM) and LE audio (BTHLEDEVICE, BTHLEENUM)
	for (const char* t : {"BTHENUM", "BTHHFENUM", "BTHLE", "BLUETOOTH"})
		if (s.find(t) != std::string::npos) return Bus::Bluetooth;
	if (s.rfind("USB|", 0) == 0 || s.find("\\USB#") != std::string::npos) return Bus::Usb;
	return Bus::Other;
}

class EndpointBuffer {
public:
	static constexpr int kGlitches = 2;
	static constexpr uint64_t kWindowNs = 30000000000ull; // 30 s

	enum class Action { None, StepUp, Shared };

	// a stream about to open on an endpoint; learned: what an earlier stream learned on it (frames,
	// -1 shared only, 0 nothing), taken when the endpoint is another than the last stream's
	void endpoint(const std::string& id, Bus bus, int learned) {
		if (id != id_ || bus != bus_) {
			id_ = id;
			bus_ = bus;
			learned_ = bus == Bus::Bluetooth ? learned : 0;
		}
		n_ = 0;
	}
	// the exclusive buffer to open with (frames): asked (0: the device's smallest), the device's
	// smallest and default periods
	int frames(int asked, int dev_min, int dev_default) {
		const int a = asked > 0 ? std::max(asked, dev_min) : dev_min;
		ceiling_ = 2 * std::max(dev_default, dev_min);
		if (bus_ != Bus::Bluetooth) return a;
		return std::max({a, dev_default, learned_});
	}
	bool shared_only() const { return bus_ == Bus::Bluetooth && learned_ < 0; }
	// a glitch of the endpoint at t (ns) in an exclusive stream of frames
	Action glitch(uint64_t t, int frames) {
		if (bus_ != Bus::Bluetooth) return Action::None;
		t_[n_++ % kGlitches] = t;
		if (n_ < kGlitches || t - t_[n_ % kGlitches] > kWindowNs) return Action::None;
		n_ = 0;
		tried_ = frames;
		if (frames * 2 > ceiling_) {
			learned_ = -1;
			return Action::Shared;
		}
		learned_ = frames * 2;
		return Action::StepUp;
	}
	// the stream's line for the user ("" none): why the buffer is what it is, or why it plays shared
	std::string note(bool exclusive_wanted, int frames) const {
		if (bus_ != Bus::Bluetooth || !exclusive_wanted) return {};
		if (learned_ < 0)
			return "Bluetooth output: exclusive mode glitched" + (tried_ > 0 ? " up to " + std::to_string(tried_) + " frames" : std::string()) +
			    ", playing shared";
		if (learned_ > 0) return "Bluetooth output: the buffer raised to " + std::to_string(frames) + " frames after glitches";
		return "Bluetooth output: the buffer at least the device's period (" + std::to_string(frames) + " frames)";
	}
	const std::string& id() const { return id_; }
	Bus bus() const { return bus_; }
	int learned() const { return learned_; }

private:
	std::string id_;
	Bus bus_ = Bus::Other;
	int learned_ = 0, ceiling_ = 0, tried_ = 0;
	uint64_t t_[kGlitches] = {};
	int n_ = 0;
};

} // namespace awva_app
