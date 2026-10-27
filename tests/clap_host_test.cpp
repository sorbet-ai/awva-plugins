// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A minimal CLAP host for the awva plugins: loads a plugin (a .clap), plays it, checks what it
// gives. Never opens an audio device: it only calls process().
//
//   clap_host_test info PLUGIN.clap COMPONENT_LIB [PRESET_DIR]
//     The plugin is its component's: id, name and summary, the parameters (ids, ranges, kinds),
//     the presets (preset-discovery, preset-load), the voices; the ports; a state round trip.
//   clap_host_test voices PLUGIN.clap
//     Prints the plugin's voice count (for the reference render's --nv).
//   clap_host_test perf PLUGIN.clap REF.f32
//     The SDK's default scripted performance through the plugin (its voice-controls test
//     extension: the controls of every block, as awva_render gives them to the engine), in host
//     blocks of 256, 1, 33 and random sizes: the output must be awva_render's REF (mono, unscaled)
//    , subnormals flushed, bit for bit, on both channels.
//   clap_host_test midifile PLUGIN.clap FILE.mid REF.f32 [RATE]
//     A Standard MIDI File in the MIDI dialect, each event at its frame (its time rounded to the
//     nearest frame, as awva_render places it), in host blocks of 256, 1, 33 and random sizes:
//     the output must be awva_render --midi's REF (mono, unscaled), bit for bit.
//   clap_host_test ump PLUGIN.clap COMPONENT_LIB
//     A MIDI 2.0 performance (MIDI2 dialect) and a MIDI 1.0 one (MIDI dialect), polyphonic and
//     mono, in host blocks of 256, 1, 33 and random sizes at 48 and 44.1 kHz, against the engine's
//     own component library given the same UMP at the same frames, block by block: bit for bit
//     (an event after its block was rendered goes to the next as a late event: plugin_audio.cpp).
//   clap_host_test notes PLUGIN.clap
//     (and:) 20 overlapping CLAP notes on 20 keys, each with its own brightness: every one sounds
//     (without it the output differs), every one's brightness is its own (changing it changes the
//     output, for an engine that reads y), and each gets exactly one NOTE_END, with its id.
//     A CLAP-dialect performance (notes with ids, pressure, tuning, brightness expressions):
//     the same as its MIDI 2.0 translation, bit for bit; the same for every host block size; it
//     sounds, the expressions change it, and a NOTE_END comes for every note.
//   clap_host_test expressive PLUGIN.clap COMPONENT_LIB DIR RATE
//     The expressive performances (tests/expressive.h) through every CLAP dialect, in host blocks
//     of 256, 1, 33 and random sizes: per_note as CLAP notes with pressure, tuning and brightness
//     expressions and as MIDI 2.0 (MIDI2 dialect), mpe as MIDI 1.0 (MIDI dialect). Each must equal
//     awva_render --ftz --ump's render of its UMP stream (DIR/ref_<performance>_RATE.f32),
//     bit for bit.
//   clap_host_test bench PLUGIN.clap COMPONENT_LIB
//     The CPU load at 48 kHz in host blocks of 128 (tests/expressive.h bench: 1, 4 and the plugin's
//     voices held, their expressions moving), the plugin's and its bare component's (the same UMP
//     into process()), the best of 5 runs of 4 s; and the latency: the plugin's reported latency
//     and the frames from a Note On to the first sample that isn't 0, over 20 phases of the 2 ms
//     step. Lines "bench clap|component VOICES LOAD%", "latency clap REPORTED MIN MAX".
//   clap_host_test stress PLUGIN.clap [SECONDS [SEED]]
//     A long run as a host gives it (default 20 s of audio): the sample rate and the block size
//     changed mid-play, random host blocks, notes with expressions, a parameter value at every
//     sample, state round trips while it plays and into a new instance; the output finite and
//     under the contract's blow-up bound (64), the states the same bytes again.
#include "clap_host.h"

using namespace host_test;

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: clap_host_test info|voices|perf|midifile|ump|notes|many|expressive|bench|stress PLUGIN.clap [...]\n");
		return 2;
	}
	const std::string mode = argv[1];
	entry = load_entry(argv[2]);
	if (!entry || !entry->init(argv[2])) {
		std::fprintf(stderr, "clap_host_test: cannot load %s\n", argv[2]);
		return 2;
	}
	factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	int rc = 0;
	if (mode == "voices") {
		std::printf("%u\n", plugin_voices());
		entry->deinit();
		return 0;
	}
	if (mode == "info" && argc >= 4) rc = info_test(argv[3], argc >= 5 ? argv[4] : nullptr);
	else if (mode == "perf" && argc >= 4) rc = perf_test(argv[3]);
	else if (mode == "ump" && argc >= 4) rc = ump_test(argv[3]);
	else if (mode == "midifile" && argc >= 5) rc = midifile_test(argv[3], argv[4], argc >= 6 ? std::atoi(argv[5]) : 48000);
	else if (mode == "notes") rc = notes_test();
	else if (mode == "many" && argc >= 4) rc = many_test(argv[3]);
	else if (mode == "expressive" && argc >= 6) rc = expressive_test(argv[3], argv[4], std::atoi(argv[5]));
	else if (mode == "bench" && argc >= 4) rc = bench_test(argv[3]);
	else if (mode == "stress") rc = stress_test(argc >= 4 ? std::atof(argv[3]) : 20, argc >= 5 ? static_cast<uint32_t>(std::atoi(argv[4])) : 1);
	else {
		std::fprintf(stderr, "clap_host_test: unknown mode %s, or arguments missing\n", mode.c_str());
		return 2;
	}
	entry->deinit();
	if (rc) return rc;
	std::printf(failures ? "clap_host_test: %d FAILED\n" : "clap_host_test: all passed\n", failures);
	return failures ? 1 : 0;
}
