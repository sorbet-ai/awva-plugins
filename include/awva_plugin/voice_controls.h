/* SPDX-FileCopyrightText: 2026 Sorbet AI
 * SPDX-License-Identifier: Apache-2.0
 *
 * A private CLAP extension of the awva plugins, for tests: the voices' controls of every block from
 * the host, instead of the plugin's notes. With a source set, the plugin renders each 32-frame
 * block with the component's process_voices() (nsc.h) on the controls the source gives, so a test
 * host can play the SDK's scripted performances through the plugin and compare its output with
 * awva_render's, bit for bit. A DAW never asks for it, and nothing changes without it.
 */
#ifndef AWVA_PLUGIN_VOICE_CONTROLS_H_
#define AWVA_PLUGIN_VOICE_CONTROLS_H_

#include <clap/clap.h>

#include "nsc/nsc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWVA_EXT_VOICE_CONTROLS "com.sorbet-ai.awva.voice-controls/1"

typedef struct awva_voice_source {
	void* ctx;
	/* the controls of block `block` (counted from activation): voices[0 .. num_voices) and the
	 * globals; called on the audio thread, before the block is rendered */
	void (*block)(void* ctx, uint64_t block, nsc_voice* voices, int32_t num_voices, nsc_globals* globals);
} awva_voice_source;

typedef struct awva_plugin_voice_controls {
	/* main thread, the plugin inactive: from now on the blocks' controls come from source (copied);
	 * NULL: from the notes again */
	bool(CLAP_ABI* set_source)(const clap_plugin_t* plugin, const awva_voice_source* source);
} awva_plugin_voice_controls;

#ifdef __cplusplus
}
#endif

#endif /* AWVA_PLUGIN_VOICE_CONTROLS_H_ */
