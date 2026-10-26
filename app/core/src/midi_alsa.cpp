// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// MIDI in on Linux: the ALSA sequencer. The app is a client with one input port, subscribed to every
// readable port of the other clients (the system's announce and timer ports aside), so a USB
// controller and a software one alike play it. Its messages arrive as sequencer events and go in as
// MIDI 1.0 in UMP (type 2: notes, poly and channel pressure, controllers, program, pitch bend), which
// the component up-converts, MPE included. Subscriptions follow refresh(): new ports subscribed,
// gone or turned-off ones dropped.
#if defined(__linux__)
#include <alsa/asoundlib.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "midi.h"
#include "platform.h"

namespace awva_app {

namespace {

class AlsaMidiIn : public MidiIn {
public:
	~AlsaMidiIn() override { stop(); }
	void start(Sink sink) override {
		sink_ = std::move(sink);
		if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
			seq_ = nullptr;
			return;
		}
		snd_seq_set_client_name(seq_, "awva");
		me_ = snd_seq_client_id(seq_);
		port_ = snd_seq_create_simple_port(seq_, "awva in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
		    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
		if (pipe(wake_) != 0) wake_[0] = wake_[1] = -1;
		run_ = true;
		th_ = std::thread([this] { loop(); });
		refresh();
	}
	void stop() override {
		if (!seq_) return;
		run_ = false;
		if (wake_[1] >= 0) (void)!write(wake_[1], "x", 1);
		if (th_.joinable()) th_.join();
		snd_seq_close(seq_);
		seq_ = nullptr;
		for (int fd : wake_)
			if (fd >= 0) close(fd);
		wake_[0] = wake_[1] = -1;
		open_.clear();
	}
	void refresh() override {
		if (!seq_) return;
		// the readable ports now, "client:port name"
		std::map<std::string, snd_seq_addr_t> now;
		snd_seq_client_info_t* ci;
		snd_seq_port_info_t* pi;
		snd_seq_client_info_alloca(&ci);
		snd_seq_port_info_alloca(&pi);
		snd_seq_client_info_set_client(ci, -1);
		while (snd_seq_query_next_client(seq_, ci) >= 0) {
			const int c = snd_seq_client_info_get_client(ci);
			if (c == SND_SEQ_CLIENT_SYSTEM || c == me_) continue;
			snd_seq_port_info_set_client(pi, c);
			snd_seq_port_info_set_port(pi, -1);
			while (snd_seq_query_next_port(seq_, pi) >= 0) {
				const unsigned caps = snd_seq_port_info_get_capability(pi);
				if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) continue;
				if (caps & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
				std::string name = std::string(snd_seq_client_info_get_name(ci)) + ": " + snd_seq_port_info_get_name(pi);
				for (int k = 2; now.count(name); k++) name = std::string(snd_seq_client_info_get_name(ci)) + ": " + snd_seq_port_info_get_name(pi) + " " + std::to_string(k);
				now[name] = *snd_seq_port_info_get_addr(pi);
			}
		}
		std::lock_guard<std::mutex> g(m_);
		for (auto it = open_.begin(); it != open_.end();) {
			const auto f = now.find(it->first);
			if (f == now.end() || disabled_.count(it->first) || f->second.client != it->second.addr.client || f->second.port != it->second.addr.port) {
				snd_seq_disconnect_from(seq_, port_, it->second.addr.client, it->second.addr.port);
				it = open_.erase(it);
			} else {
				++it;
			}
		}
		std::vector<MidiPort> listed;
		for (const auto& [name, addr] : now) {
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
					for (auto& [n, o] : open_) used = used || o.slot == s;
					if (!used) slot = s;
				}
				if (slot >= 0 && snd_seq_connect_from(seq_, port_, addr.client, addr.port) >= 0) {
					open_[name] = {addr, slot};
					p.state = "open";
				} else {
					p.state = "failed";
					p.message = "the port can't be subscribed to";
				}
			}
			listed.push_back(p);
		}
		listed_ = listed;
	}
	std::vector<MidiPort> ports() const override {
		std::lock_guard<std::mutex> g(m_);
		return listed_;
	}

private:
	struct Open {
		snd_seq_addr_t addr;
		int slot;
	};
	int slot_of(const snd_seq_addr_t& a) {
		std::lock_guard<std::mutex> g(m_);
		for (auto& [n, o] : open_)
			if (o.addr.client == a.client && o.addr.port == a.port) return o.slot;
		return -1;
	}
	void loop() {
		const int n = snd_seq_poll_descriptors_count(seq_, POLLIN);
		std::vector<pollfd> fds(static_cast<size_t>(n) + 1);
		snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(n), POLLIN);
		fds[static_cast<size_t>(n)] = {wake_[0], POLLIN, 0};
		while (run_) {
			if (poll(fds.data(), fds.size(), 500) <= 0) continue;
			snd_seq_event_t* ev = nullptr;
			while (run_ && snd_seq_event_input_pending(seq_, 1) > 0 && snd_seq_event_input(seq_, &ev) >= 0 && ev) {
				const uint64_t t = now_ns();
				uint32_t w = 0;
				const auto& c = ev->data.control;
				const auto& nt = ev->data.note;
				switch (ev->type) {
				case SND_SEQ_EVENT_NOTEON: w = 0x20900000u | (nt.channel & 15u) << 16 | (nt.note & 127u) << 8 | (nt.velocity & 127u); break;
				case SND_SEQ_EVENT_NOTEOFF: w = 0x20800000u | (nt.channel & 15u) << 16 | (nt.note & 127u) << 8 | (nt.off_velocity & 127u); break;
				case SND_SEQ_EVENT_KEYPRESS: w = 0x20a00000u | (nt.channel & 15u) << 16 | (nt.note & 127u) << 8 | (nt.velocity & 127u); break;
				case SND_SEQ_EVENT_CONTROLLER: w = 0x20b00000u | (c.channel & 15u) << 16 | (c.param & 127u) << 8 | (c.value & 127u); break;
				case SND_SEQ_EVENT_PGMCHANGE: w = 0x20c00000u | (c.channel & 15u) << 16 | (c.value & 127u) << 8; break;
				case SND_SEQ_EVENT_CHANPRESS: w = 0x20d00000u | (c.channel & 15u) << 16 | (c.value & 127u) << 8; break;
				case SND_SEQ_EVENT_PITCHBEND: {
					const int v = c.value + 8192; // (-8192 .. 8191 -> 14 bits)
					w = 0x20e00000u | (c.channel & 15u) << 16 | static_cast<uint32_t>(v & 127) << 8 | static_cast<uint32_t>((v >> 7) & 127);
					break;
				}
				default: break;
				}
				if (!w) continue;
				const int slot = slot_of(ev->source);
				if (slot >= 0) sink_(slot, &w, 1, t);
			}
		}
	}

	Sink sink_;
	snd_seq_t* seq_ = nullptr;
	int me_ = -1, port_ = -1;
	int wake_[2] = {-1, -1};
	std::atomic<bool> run_{false};
	std::thread th_;
	std::map<std::string, Open> open_;
	mutable std::mutex m_;
	std::vector<MidiPort> listed_;
};

} // namespace

std::unique_ptr<MidiIn> make_midi_in() { return std::make_unique<AlsaMidiIn>(); }

} // namespace awva_app
#endif
