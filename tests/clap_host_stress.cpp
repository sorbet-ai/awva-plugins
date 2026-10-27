// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// clap_host_test's stress mode (clap_host.h): a long run as a host gives it.
#include "clap_host.h"

namespace host_test {

// a long run as a host gives it, seeded: segments of about two seconds, each at another sample rate
// and max block size (deactivated and activated again mid-play, notes held across it), its host
// blocks random sizes up to that max; notes coming and going with pressure and brightness; every
// sample a parameter value event (each parameter in turn: random values, a tenth of them its min
// or max); every segment a state round trip while it plays (saved, loaded, saved again: the same
// bytes) and every third a state loaded into a new instance that saves the same bytes. The output
// must stay finite and under the awva contract's blow-up bound (64: extreme but legal parameters may
// be loud, docs/conformance.md); the peak is printed.
constexpr float kBlowUp = 64.0f;
int stress_test(double seconds, uint32_t seed) {
	std::mt19937 rng(seed);
	Instance in;
	if (!in.p) return check(false, "stress: the plugin is created"), 1;
	const auto* pa = in.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
	const auto* st = in.ext<clap_plugin_state>(CLAP_EXT_STATE);
	if (!pa || !st) return check(false, "stress: params and state"), 1;
	std::vector<clap_param_info> params(pa->count(in.p));
	for (uint32_t i = 0; i < params.size(); i++) pa->get_info(in.p, i, &params[i]);
	const double rates[] = {48000, 44100, 96000, 22050, 192000, 88200, 32000};
	const uint32_t blocks[] = {4096, 1, 64, 1024, 256, 17, 512};
	struct Held {
		int32_t id;
		int16_t key;
		uint64_t off;
	};
	std::vector<Held> held;
	int32_t next_id = 1;
	uint64_t frames = 0, calls = 0, param_events = 0, trips = 0, fresh = 0, bad = 0, over = 0;
	double played = 0, peak = 0;
	std::vector<float> l(4096), r(4096);
	auto save = [&](const clap_plugin* p, Buffer* b) {
		const clap_ostream os = {b, buf_write};
		return st->save(p, &os);
	};
	for (int seg = 0; played < seconds; seg++) {
		const double rate = rates[seg % 7];
		const uint32_t max_block = blocks[(seg / 7 + seg) % 7];
		if (in.active) {
			in.p->stop_processing(in.p);
			in.p->deactivate(in.p);
		}
		in.active = in.p->activate(in.p, rate, 1, max_block);
		if (!in.active) {
			check(false, "stress: activates at " + std::to_string(rate) + " Hz, blocks up to " + std::to_string(max_block));
			return 1;
		}
		in.p->start_processing(in.p);
		const uint64_t seg_frames = static_cast<uint64_t>(2 * rate);
		const uint64_t trip_at = seg_frames / 2;
		bool tripped = false;
		for (uint64_t at = 0; at < seg_frames;) {
			const uint32_t n = std::min<uint64_t>(seg_frames - at, rng() % 3 == 0 ? max_block : 1 + rng() % max_block);
			Events ev;
			// notes: one in about every quarter second, held 0.05 .. 1 s, pressure and brightness moving
			for (auto it = held.begin(); it != held.end();) {
				if (it->off < frames + n) {
					clap_event_note x = {{sizeof x, static_cast<uint32_t>(it->off > frames ? it->off - frames : 0), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0},
					    it->id, 0, 0, it->key, 0.0};
					ev.add(x);
					it = held.erase(it);
				} else {
					clap_event_note_expression e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0},
					    rng() % 2 ? CLAP_NOTE_EXPRESSION_PRESSURE : CLAP_NOTE_EXPRESSION_BRIGHTNESS, it->id, 0, 0, it->key, (rng() % 1001) / 1000.0};
					ev.add(e);
					++it;
				}
			}
			if (rng() % static_cast<uint32_t>(std::max(1.0, rate / 4 / n)) == 0 && held.size() < 16) {
				const Held h = {next_id++, static_cast<int16_t>(36 + rng() % 60), frames + static_cast<uint64_t>(rate * (0.05 + (rng() % 951) / 1000.0))};
				clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, h.id, 0, 0, h.key, 0.2 + (rng() % 801) / 1000.0};
				ev.add(x);
				held.push_back(h);
			}
			// a parameter value at every sample
			for (uint32_t k = 0; k < n && !params.empty(); k++) {
				const clap_param_info& pi = params[param_events % params.size()];
				const uint32_t u = rng() % 20;
				double v = u == 0 ? pi.min_value : (u == 1 ? pi.max_value : pi.min_value + (pi.max_value - pi.min_value) * (rng() % 10001) / 10000.0);
				if (pi.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
				clap_event_param_value e = {{sizeof e, k, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, pi.id, pi.cookie, -1, -1, -1, -1, v};
				ev.add(e);
				param_events++;
			}
			// (the events in time order: notes at their frames first, then the values)
			ev.seal();
			std::stable_sort(ev.list.begin(), ev.list.end(), [](const clap_event_header* a, const clap_event_header* b) { return a->time < b->time; });
			clap_input_events ie = ev.in();
			float* chans[2] = {l.data(), r.data()};
			clap_audio_buffer ob = {chans, nullptr, 2, 0, 0};
			clap_process pr = {static_cast<int64_t>(frames), n, nullptr, nullptr, &ob, 0, 1, &ie, &kOut};
			in.p->process(in.p, &pr);
			calls++;
			for (uint32_t i = 0; i < n; i++)
				for (const float x : {l[i], r[i]}) {
					if (!std::isfinite(x)) bad++;
					else {
						peak = std::max(peak, static_cast<double>(std::fabs(x)));
						if (std::fabs(x) > kBlowUp) over++;
					}
				}
			at += n;
			frames += n;
			// a state round trip while it plays (the host's main thread between two blocks)
			if (!tripped && at >= trip_at) {
				tripped = true;
				Buffer a, b;
				bool ok = save(in.p, &a);
				Buffer ra{a.data};
				const clap_istream is = {&ra, buf_read};
				ok = ok && st->load(in.p, &is) && save(in.p, &b);
				if (!ok || a.data != b.data) check(false, "stress: segment " + std::to_string(seg) + ": the state saved, loaded and saved again is the same");
				trips++;
				if (seg % 3 == 0) {
					Instance other;
					Buffer rb{a.data}, c;
					const clap_istream is2 = {&rb, buf_read};
					if (!other.p || !st->load(other.p, &is2) || !save(other.p, &c) || c.data != a.data)
						check(false, "stress: segment " + std::to_string(seg) + ": a new instance loads the state and saves it the same");
					fresh++;
				}
			}
		}
		played += 2;
	}
	char s[400];
	std::snprintf(s, sizeof s,
	    "stress: %.0f s at 7 rates, blocks up to 1..4096 (%llu process calls), %llu parameter values (one a sample), %llu state round trips (%llu into a "
	    "new instance): %llu samples not finite, %llu past %.0f (peak %.3f)",
	    played, static_cast<unsigned long long>(calls), static_cast<unsigned long long>(param_events), static_cast<unsigned long long>(trips),
	    static_cast<unsigned long long>(fresh), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(over), static_cast<double>(kBlowUp), peak);
	check(bad == 0 && over == 0, s);
	return 0;
}

} // namespace host_test
