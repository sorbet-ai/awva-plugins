// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The expressive performances of the format proofs, and the UMP streams the plugins give the engine
// for them (plugin.cpp's mappings). Every host test (clap_host_test, vst3_host_test, au_host_test)
// plays a performance through its format's own events, and the output must equal awva_render's
// render of the UMP stream (make_expressive writes them), bit for bit.
//
//   per_note   five overlapping notes (on channels 1, 3 and 6, two of them on the same key), each
//              with its own pressure, tuning and brightness (from its Note On). Every value is exact in every format:
//              velocities 16-bit, pressures and brightnesses k/1024 (exact in a VST3 float),
//              tunings multiples of 15/4096 semitone (exact through VST3's 0..1 = -120..120 st).
//              Its UMP: MIDI 2.0 Note On/Off (16-bit velocities), Poly Pressure, Registered
//              Per-Note Controller 3 (Pitch 7.25: the key plus the tuning), Registered Per-Note
//              Controller 74 (brightness).
//   mpe        MPE: the bend range (RPN 0) set to 12 on a member channel, five notes on member
//              channels 2 .. 6 with 7-bit velocities, and each channel's pitch bend (14-bit),
//              channel pressure and CC74, and the master channel's bend. Its UMP: MIDI 1.0 (type 2).
//   mpe_vst3   the same through VST3, whose notes carry 32-bit float velocities: the plugin gives them
//              as MIDI 2.0 Note On/Off with 16-bit velocities (the rest is MIDI 1.0 as above).
//   mpe_midi2  the same through an AU, which asks for MIDI 2.0: the system translates MIDI 1.0 to
//              MIDI 2.0 by the UMP spec's default translation (values scaled up min-center-max, the
//              RPN as a Registered Controller), and the AU gets that.
// Frames are at the rate's own scale (48 kHz times rate / 48000).
#ifndef AWVA_PLUGINS_TESTS_EXPRESSIVE_H_
#define AWVA_PLUGINS_TESTS_EXPRESSIVE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace expressive {

enum Kind : uint8_t {
	kNoteOn,       // channel, key, id; vel16 (per_note) or vel7 (mpe)
	kNoteOff,      // channel, key, id; the release velocity, as kNoteOn's
	kPressure,     // channel, key, id; k1024: the pressure k/1024
	kTuning,       // channel, key, id; tune: the tuning tune * 15/4096 semitones
	kBrightness,   // channel, key, id; k1024: the brightness k/1024
	kBend,         // channel; value: 14-bit, 8192 the centre
	kChanPressure, // channel; value: 7-bit
	kControl,      // channel; cc, value: 7-bit
};
struct Event {
	uint64_t frame;
	Kind kind;
	uint8_t channel = 0, key = 0, cc = 0;
	int32_t id = -1;     // the note's id (CLAP, VST3), -1 for the channel messages
	int32_t value = 0;   // as the kind says
	bool midi1 = false;  // (a note of mpe: 7-bit velocities)
};

// the values as the formats carry them
inline double velocity(const Event& e) { return e.midi1 ? e.value / 127.0 : e.value / 65535.0; }
inline double unit(const Event& e) { return e.value / 1024.0; }                 // pressure, brightness
inline double semitones(const Event& e) { return e.value * 15.0 / 4096.0; }     // tuning
inline double vst3_tuning(const Event& e) { return 0.5 + e.value / 65536.0; }   // = 0.5 + semitones / 240

// plugin.cpp's conversions
inline uint32_t u32_of(double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 1.0) * 4294967295.0 + 0.5); }
inline uint32_t u16_of(double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 1.0) * 65535.0 + 0.5); }

inline uint64_t at(double frames48, int rate) { return static_cast<uint64_t>(std::llround(frames48 * rate / 48000.0)); }

inline void sort(std::vector<Event>* v) {
	std::stable_sort(v->begin(), v->end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
}

inline std::vector<Event> per_note(int rate) {
	std::vector<Event> v;
	const uint8_t channels[5] = {0, 0, 2, 0, 5}, keys[5] = {55, 60, 60, 64, 67};
	for (int n = 0; n < 5; n++) {
		const uint8_t ch = channels[n], key = keys[n];
		const int32_t id = 100 + n;
		const double on = 2400 + 9000.0 * n + 37 * n;
		v.push_back({at(on, rate), kNoteOn, ch, key, 0, id, 20000 + 9000 * n});
		// (its brightness with the Note On: an engine that reads the timbre at the strike sees it)
		v.push_back({at(on, rate), kBrightness, ch, key, 0, id, 200 + 150 * n});
		for (int k = 1; k < 100; k++) {
			const double t = on + 480.0 * k + 7 * n;
			const int32_t p = static_cast<int32_t>(std::lround(1024 * (0.5 + 0.45 * std::sin(0.07 * k + n))));
			const int32_t tune = static_cast<int32_t>(std::lround(4096 / 15.0 * 0.6 * std::sin(0.05 * k + n)));
			const int32_t b = static_cast<int32_t>(std::lround(1024 * (0.5 + 0.5 * std::sin(0.03 * k + 1.0 + n))));
			v.push_back({at(t, rate), kPressure, ch, key, 0, id, p});
			v.push_back({at(t + 2, rate), kTuning, ch, key, 0, id, tune});
			v.push_back({at(t + 4, rate), kBrightness, ch, key, 0, id, b});
		}
		v.push_back({at(on + 50000, rate), kNoteOff, ch, key, 0, id, 30000 + 4000 * n});
	}
	sort(&v);
	return v;
}

inline std::vector<Event> mpe(int rate) {
	std::vector<Event> v;
	// the notes' bend range: RPN 0 = 12 semitones, on member channel 2
	v.push_back({at(10, rate), kControl, 1, 0, 101, -1, 0});
	v.push_back({at(12, rate), kControl, 1, 0, 100, -1, 0});
	v.push_back({at(14, rate), kControl, 1, 0, 6, -1, 12});
	const uint8_t keys[5] = {57, 61, 64, 69, 52};
	for (int n = 0; n < 5; n++) {
		const uint8_t ch = static_cast<uint8_t>(1 + n), key = keys[n];
		const double on = 2400 + 8000.0 * n + 41 * n;
		const int32_t id = 200 + n;
		Event e{at(on, rate), kNoteOn, ch, key, 0, id, 50 + 15 * n};
		e.midi1 = true;
		v.push_back(e);
		for (int k = 1; k < 90; k++) {
			const double t = on + 480.0 * k + 5 * n;
			const int32_t bend = 8192 + static_cast<int32_t>(std::lround(2400 * std::sin(0.06 * k + n)));
			const int32_t pres = static_cast<int32_t>(std::lround(64 + 60 * std::sin(0.08 * k + n)));
			const int32_t y = static_cast<int32_t>(std::lround(64 + 62 * std::sin(0.04 * k + 2.0 + n)));
			v.push_back({at(t, rate), kBend, ch, 0, 0, -1, bend});
			v.push_back({at(t + 2, rate), kChanPressure, ch, 0, 0, -1, pres});
			v.push_back({at(t + 4, rate), kControl, ch, 0, 74, -1, y});
		}
		Event off{at(on + 45000, rate), kNoteOff, ch, key, 0, id, 40 + 10 * n};
		off.midi1 = true;
		v.push_back(off);
	}
	for (int k = 0; k < 6; k++) // the master channel's bend: the instrument's, every note follows it
		v.push_back({at(12000 + 6000.0 * k + 3, rate), kBend, 0, 0, 0, -1, 8192 + (k % 2 ? 1500 : -700) * (k < 5)});
	sort(&v);
	return v;
}

// the CPU benchmark's performance: `voices` notes held for `seconds` (keys a fourth apart, from
// frame 0 a millisecond apart), each with its pressure, tuning and brightness moving every 10 ms
inline std::vector<Event> bench(int voices, int rate, double seconds) {
	std::vector<Event> v;
	const uint64_t end = static_cast<uint64_t>(seconds * rate);
	for (int n = 0; n < voices; n++) {
		const uint8_t key = static_cast<uint8_t>(43 + 5 * n);
		const int32_t id = 300 + n;
		const double on = 48.0 * n;
		v.push_back({at(on, rate), kNoteOn, 0, key, 0, id, 40000});
		for (int k = 1; at(on + 480.0 * k + 4, rate) < end; k++) {
			const double t = on + 480.0 * k;
			v.push_back({at(t, rate), kPressure, 0, key, 0, id, static_cast<int32_t>(std::lround(1024 * (0.6 + 0.3 * std::sin(0.05 * k + n))))});
			v.push_back({at(t + 2, rate), kTuning, 0, key, 0, id, static_cast<int32_t>(std::lround(4096 / 15.0 * 0.1 * std::sin(0.3 * k)))});
			v.push_back({at(t + 4, rate), kBrightness, 0, key, 0, id, static_cast<int32_t>(std::lround(1024 * (0.5 + 0.4 * std::sin(0.02 * k + n))))});
		}
	}
	sort(&v);
	return v;
}
// the latency measurement's performance: one note, its Note On at `frame`
inline std::vector<Event> one_note(uint64_t frame) { return {{frame, kNoteOn, 0, 60, 0, 400, 50000}}; }
// the frames from `from` to the first sample that isn't 0 (interleaved stereo; -1: none, -2: not
// silent before it)
inline int64_t onset(const std::vector<float>& out, uint64_t from) {
	for (size_t i = 0; i < out.size(); i++)
		if (out[i] != 0.0f) return i / 2 < from ? -2 : static_cast<int64_t>(i / 2 - from);
	return -1;
}

// --- UMP, as the plugin gives it to the engine ------------------------------------------------------
struct Packet {
	uint64_t frame;
	uint32_t w0, w1;
};
inline uint32_t midi2(uint8_t status, uint8_t ch, uint8_t d1, uint8_t d2) {
	return 0x40000000u | static_cast<uint32_t>(status | ch) << 16 | static_cast<uint32_t>(d1) << 8 | d2;
}
inline uint32_t midi1(uint8_t status, uint8_t ch, uint8_t d1, uint8_t d2) {
	return 0x20000000u | static_cast<uint32_t>(status | ch) << 16 | static_cast<uint32_t>(d1 & 0x7f) << 8 | (d2 & 0x7fu);
}
// vst3: the notes as VST3 gives them (float velocities, as MIDI 2.0 Note On/Off)
inline std::vector<Packet> ump(const std::vector<Event>& perf, bool vst3 = false) {
	std::vector<Packet> u;
	for (const Event& e : perf) {
		switch (e.kind) {
		case kNoteOn:
		case kNoteOff: {
			const uint8_t status = e.kind == kNoteOn ? 0x90 : 0x80;
			if (e.midi1 && !vst3) {
				u.push_back({e.frame, midi1(status, e.channel, e.key, static_cast<uint8_t>(e.value)), 0});
			} else {
				const uint32_t v16 = e.midi1 ? u16_of(static_cast<float>(velocity(e))) : static_cast<uint32_t>(e.value);
				u.push_back({e.frame, midi2(status, e.channel, e.key, 0), v16 << 16});
			}
			break;
		}
		case kPressure: u.push_back({e.frame, midi2(0xa0, e.channel, e.key, 0), u32_of(unit(e))}); break;
		case kTuning:
			u.push_back({e.frame, midi2(0x00, e.channel, e.key, 3), static_cast<uint32_t>((e.key + semitones(e)) * 33554432.0)});
			break;
		case kBrightness: u.push_back({e.frame, midi2(0x00, e.channel, e.key, 74), u32_of(unit(e))}); break;
		case kBend:
			u.push_back({e.frame, midi1(0xe0, e.channel, static_cast<uint8_t>(e.value & 0x7f), static_cast<uint8_t>(e.value >> 7)), 0});
			break;
		case kChanPressure: u.push_back({e.frame, midi1(0xd0, e.channel, static_cast<uint8_t>(e.value), 0), 0}); break;
		case kControl: u.push_back({e.frame, midi1(0xb0, e.channel, e.cc, static_cast<uint8_t>(e.value)), 0}); break;
		}
	}
	return u;
}
// the UMP spec's default MIDI 1.0 -> 2.0 value translation (min-center-max scaling)
inline uint32_t scale_up(uint32_t v, int src_bits, int dst_bits) {
	const int scale_bits = dst_bits - src_bits;
	uint32_t out = v << scale_bits;
	if (v <= (1u << (src_bits - 1))) return out;
	const int repeat_bits = src_bits - 1;
	uint32_t repeat = v & ((1u << repeat_bits) - 1);
	repeat = scale_bits > repeat_bits ? repeat << (scale_bits - repeat_bits) : repeat >> (repeat_bits - scale_bits);
	while (repeat != 0) {
		out |= repeat;
		repeat >>= repeat_bits;
	}
	return out;
}
// a MIDI 1.0 performance (mpe) as MIDI 2.0 by the default translation: Note On/Off with 16-bit
// velocities (a Note On of velocity 0 is a Note Off), pressures, bends and controllers in 32 bits,
// and RPN 101/100 + Data Entry 6 as a Registered Controller (the selects themselves not sent)
inline std::vector<Packet> ump_midi2(const std::vector<Event>& perf) {
	std::vector<Packet> u;
	int rpn_msb[16], rpn_lsb[16];
	for (int c = 0; c < 16; c++) rpn_msb[c] = rpn_lsb[c] = 0x7f;
	for (const Event& e : perf) {
		const uint32_t v = static_cast<uint32_t>(e.value);
		switch (e.kind) {
		case kNoteOn:
		case kNoteOff: {
			const bool on = e.kind == kNoteOn && v != 0;
			u.push_back({e.frame, midi2(on ? 0x90 : 0x80, e.channel, e.key, 0), scale_up(v, 7, 16) << 16});
			break;
		}
		case kBend: u.push_back({e.frame, midi2(0xe0, e.channel, 0, 0), scale_up(v, 14, 32)}); break;
		case kChanPressure: u.push_back({e.frame, midi2(0xd0, e.channel, 0, 0), scale_up(v, 7, 32)}); break;
		case kControl:
			if (e.cc == 101) rpn_msb[e.channel] = static_cast<int>(v);
			else if (e.cc == 100) rpn_lsb[e.channel] = static_cast<int>(v);
			else if (e.cc == 6 && rpn_msb[e.channel] != 0x7f && rpn_lsb[e.channel] != 0x7f)
				u.push_back({e.frame, midi2(0x20, e.channel, static_cast<uint8_t>(rpn_msb[e.channel]), static_cast<uint8_t>(rpn_lsb[e.channel])),
				    scale_up(v << 7, 14, 32)});
			else u.push_back({e.frame, midi2(0xb0, e.channel, e.cc, 0), scale_up(v, 7, 32)});
			break;
		default: break;
		}
	}
	return u;
}

// the MIDI 1.0 bytes of an mpe event (its UMP's), for a MIDI 1.0 path: 2 or 3 bytes
inline int bytes(const Event& e, uint8_t out[3]) {
	switch (e.kind) {
	case kNoteOn: out[0] = static_cast<uint8_t>(0x90 | e.channel), out[1] = e.key, out[2] = static_cast<uint8_t>(e.value); return 3;
	case kNoteOff: out[0] = static_cast<uint8_t>(0x80 | e.channel), out[1] = e.key, out[2] = static_cast<uint8_t>(e.value); return 3;
	case kBend:
		out[0] = static_cast<uint8_t>(0xe0 | e.channel), out[1] = static_cast<uint8_t>(e.value & 0x7f), out[2] = static_cast<uint8_t>(e.value >> 7);
		return 3;
	case kChanPressure: out[0] = static_cast<uint8_t>(0xd0 | e.channel), out[1] = static_cast<uint8_t>(e.value), out[2] = 0; return 2;
	case kControl: out[0] = static_cast<uint8_t>(0xb0 | e.channel), out[1] = e.cc, out[2] = static_cast<uint8_t>(e.value); return 3;
	default: return 0;
	}
}

// the length of a render of the performance: its last event, the engine's tail (at most 10 s) and 1 s
inline uint64_t frames(const std::vector<Event>& perf, int rate, double tail_seconds) {
	return perf.back().frame + static_cast<uint64_t>((std::min(tail_seconds, 10.0) + 1.0) * rate);
}

inline bool write_ump(const std::string& path, const std::vector<Packet>& u) {
	FILE* f = std::fopen(path.c_str(), "w");
	if (!f) return false;
	std::fprintf(f, "# frame word0 word1 (tests/expressive.h)\n");
	for (const Packet& p : u) std::fprintf(f, "%llu %08x %08x\n", static_cast<unsigned long long>(p.frame), p.w0, p.w1);
	return std::fclose(f) == 0;
}

// a float32 file (awva_render --out)
inline std::vector<float> read_f32(const std::string& path) {
	std::vector<float> v;
	if (FILE* f = std::fopen(path.c_str(), "rb")) {
		float buf[4096];
		size_t got;
		while ((got = std::fread(buf, 4, 4096, f)) > 0) v.insert(v.end(), buf, buf + got);
		std::fclose(f);
	}
	return v;
}

// a plugin's stereo output (interleaved) against a mono reference, subnormals flushed, as
// the plugins give it: the frames that differ (both channels must be it), the first, the largest
// difference and the reference's peak
struct Diff {
	size_t bad = 0, first = 0;
	double max_diff = 0, peak = 0;
};
inline float flush(float x) { return x != 0.0f && std::fabs(x) < 1.17549435e-38f ? std::copysign(0.0f, x) : x; }
inline Diff differ(const std::vector<float>& out, const std::vector<float>& ref) {
	Diff d;
	for (size_t i = 0; i < ref.size() && 2 * i + 1 < out.size(); i++) {
		const float want = flush(ref[i]);
		d.peak = std::fmax(d.peak, std::fabs(want));
		bool same = true;
		for (int c = 0; c < 2; c++) {
			const float got = out[2 * i + c];
			if (std::memcmp(&got, &want, 4) == 0) continue;
			same = false;
			d.max_diff = std::fmax(d.max_diff, std::fabs(static_cast<double>(got) - want));
		}
		if (!same && !d.bad++) d.first = i;
	}
	if (out.size() != 2 * ref.size()) d.bad += ref.size();
	return d;
}

} // namespace expressive

#endif // AWVA_PLUGINS_TESTS_EXPRESSIVE_H_
