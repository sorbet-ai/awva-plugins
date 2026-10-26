/* SPDX-FileCopyrightText: 2026 Sorbet AI
 * SPDX-License-Identifier: Apache-2.0
 *
 * The awva app's core as a C ABI: what the Flutter UI calls through dart:ffi. The UI holds no
 * logic and no DSP; everything it shows comes from here, and everything it does goes through here.
 *
 *   engines   the built-in engines (nsc components beside the core) and the user's packages
 *             (awva-package/1 folders, app/README.md), listed by awva_catalog()
 *   the sound one engine instance, driven as the plugins drive theirs (plugin.cpp): UMP in at
 *             integer frames, 32-frame blocks, x0.25, subnormals flushed
 *   MIDI in   every input port, MIDI 1.0 and MPE as MIDI 1.0 in UMP (the component up-converts),
 *             MIDI 2.0 UMP where the OS gives it (CoreMIDI)
 *   audio out WASAPI shared mode (Windows), CoreAudio (macOS), or a null sink (no_audio)
 *   safety    the master level (it starts at most at -12 dB and fades in), a true-peak limiter at
 *             -1 dBTP with a 1.4 ms look-ahead, a guard against non-finite samples, a hard ceiling
 *
 * Threads: every function here is for the UI thread (one thread), except awva_status(), which any
 * thread may call. Text results are UTF-8 JSON, valid until the next call of the same function.
 */
#ifndef AWVA_APP_H_
#define AWVA_APP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define AWVA_APP_API __declspec(dllexport)
#else
#define AWVA_APP_API __attribute__((visibility("default")))
#endif

#define AWVA_APP_ABI 1

/* audio states */
enum { AWVA_AUDIO_OFF = 0, AWVA_AUDIO_RUNNING = 1, AWVA_AUDIO_NULL = 2, AWVA_AUDIO_FAILED = 3 };

typedef struct awva_status {
	float peak_l, peak_r;   /* the output's peaks since the last call, 0 .. 1 (post safety) */
	float reduction_db;     /* the limiter's largest gain reduction since the last call, dB (>= 0) */
	float master_db;        /* the master level now */
	float cpu;              /* render time / buffer time, the last second's peak, 0 .. 1+ */
	int32_t voices;         /* the engine's active voices */
	int32_t audio_state;    /* AWVA_AUDIO_* */
	int32_t sample_rate;
	int32_t buffer_frames;
	uint32_t midi_events;   /* MIDI messages received, ever */
	uint32_t generation;    /* changes when the sound's values changed (a preset, a load): read them again */
	uint32_t xruns;         /* buffers the audio thread was late for */
	uint32_t faults;        /* blocks muted for a broken engine output (non-finite, or past the awva
	                           contract's bound of 8): the engine was reset */
	int32_t engine_stopped; /* 1: the package's engine broke the bound and is stopped (choose another) */
} awva_status;

AWVA_APP_API int32_t awva_abi(void);

/* start the core. options: JSON, every key optional:
 *   "no_audio": true          the null sink (nothing opens an audio device)
 *   "engines_dir": "<path>"   the user's packages (default: %APPDATA%\awva\engines,
 *                             ~/Library/Application Support/awva/engines)
 *   "builtin_dir": "<path>"   the built-in engines (default: beside the core)
 *   "settings": "<path>"      the settings file (default: <app data>/awva/settings.json; "" none)
 *   "probe_wav": "<path>"     every output sample, post safety, also into this WAV (float 32)
 *   "midi": false             no MIDI ports opened
 * Returns 0, or -1 if it was started already. */
AWVA_APP_API int32_t awva_start(const char* options_json);
AWVA_APP_API void awva_stop(void);

/* the engines: [{"key","label","source","id","title","version","summary","colour","ok","error"}...] */
AWVA_APP_API const char* awva_catalog(void);
/* the packages folder the core scans */
AWVA_APP_API const char* awva_engines_dir(void);
/* scan the packages folder again; returns the number of engines */
AWVA_APP_API int32_t awva_rescan(void);
/* play an engine by its catalog key; 0, or -1 (unknown or refused) */
AWVA_APP_API int32_t awva_select_engine(const char* key);

/* the sound: {"engine": {...}, "presets": [{"name","source"}...], "preset": i, "heroes": [param
 * indices], "params": [{"name","group","min","max","kind","def","value"}...], "legato": bool} */
AWVA_APP_API const char* awva_sound(void);
/* a preset of the list awva_sound() gives; 0, or -1 (refused: awva_last_error()) */
AWVA_APP_API int32_t awva_select_preset(int32_t index);
AWVA_APP_API int32_t awva_set_param(int32_t index, float value);
AWVA_APP_API float awva_param(int32_t index);
AWVA_APP_API void awva_set_legato(int32_t on);
AWVA_APP_API void awva_panic(void); /* every note off, the engine reset */

/* the master level, dB (-60 .. +6; -60 is silence) */
AWVA_APP_API void awva_set_master_db(float db);
AWVA_APP_API void awva_status_get(awva_status* out);

/* MIDI: [{"name","enabled","state":"open|closed|busy|failed","message"}...] */
AWVA_APP_API const char* awva_midi_ports(void);
AWVA_APP_API void awva_midi_refresh(void); /* the OS's ports again, and the audio onto a new default
                                            output (call every second or two) */
AWVA_APP_API void awva_midi_enable(const char* name, int32_t on);
/* UMP words straight into the sound, as from a port (tests, smoke runs) */
AWVA_APP_API void awva_ump(const uint32_t* words, int32_t count);
/* MIDI 1.0 bytes straight into the sound, as from a port (one or more whole messages) */
AWVA_APP_API void awva_midi1(const uint8_t* bytes, int32_t count);

/* audio: {"backend","device","state","message","rate","buffer"} */
AWVA_APP_API const char* awva_audio(void);
AWVA_APP_API int32_t awva_audio_restart(void);

/* settings the UI keeps (e.g. "theme"): a string, "" when unset */
AWVA_APP_API const char* awva_setting(const char* key);
AWVA_APP_API void awva_set_setting(const char* key, const char* value);

AWVA_APP_API const char* awva_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* AWVA_APP_H_ */
