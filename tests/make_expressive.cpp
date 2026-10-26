// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// make_expressive DIR RATE: the UMP streams of the expressive performances (tests/expressive.h), as
// awva_render --ump reads them: DIR/per_note_RATE.ump, DIR/mpe_RATE.ump, DIR/mpe_vst3_RATE.ump,
// DIR/mpe_midi2_RATE.ump.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "expressive.h"

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: make_expressive DIR RATE\n");
		return 2;
	}
	const std::string dir = argv[1];
	const int rate = std::atoi(argv[2]);
	const std::string r = std::to_string(rate);
	using namespace expressive;
	const bool ok = write_ump(dir + "/per_note_" + r + ".ump", ump(per_note(rate))) && write_ump(dir + "/mpe_" + r + ".ump", ump(mpe(rate))) &&
	    write_ump(dir + "/mpe_vst3_" + r + ".ump", ump(mpe(rate), true)) && write_ump(dir + "/mpe_midi2_" + r + ".ump", ump_midi2(mpe(rate)));
	if (!ok) std::fprintf(stderr, "make_expressive: cannot write in %s\n", dir.c_str());
	return ok ? 0 : 1;
}
