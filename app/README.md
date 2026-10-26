# awva, the standalone app

The awva engines as an instrument of their own, for Windows and macOS: pick an engine and a
preset, shape it with its hero knobs, play it from any MIDI 2.0, MPE or MIDI 1.0 controller. The
same design is what the plugins show in a DAW, drawn natively (`DESIGN.md`).

- **`core/`**: the engine, MIDI, audio and safety, in C++ (`core/include/awva_app/awva_app.h`, a C
  ABI). It is built by this repository's own build, with the plugins' toolchain and awva, so the
  app plays what the plugins play: `ctest -L app` checks every engine's output against its CLAP's,
  bit for bit.
- **`lib/`**: the UI, in Flutter. It calls the core through `dart:ffi` and holds no logic: what
  it shows comes from the core, and what the user does goes to the core.

## What it does

- **Engines:** the built-in engines (awva's component libraries, beside the core), and your own
  engine packages from a folder (below), listed by their labels.
- **Presets:** the engine's factory presets, and a package's preset files.
- **The heroes:** the preset's hero parameters (else the engine's) as knobs; every parameter on
  request; mono.
- **MIDI in:** every input port the OS has (turn one off in Settings).
  - Windows: WinMM. Where Windows MIDI Services runs (Windows 11), ports are multi-client, so another
    app can have the controller open at the same time. Without it a USB port is single-client: a
    port another app holds is shown as busy (footer and Settings) with what to do.
  - macOS: CoreMIDI in the MIDI 2.0 protocol (UMP; a MIDI 1.0 controller is up-converted by the OS).
  - MPE as the MPE spec has it: the zone from the MPE Configuration Message, each note's pitch bend
    at the bend range the controller sets (RPN 0, 48 semitones by default), channel pressure (or
    poly pressure), CC74. MIDI 2.0: per-note pitch, pressure and timbre. All of it is the engine
    component's own decoding (awva's `midi_in.h`), as in the plugins. A new engine is given the
    controller's MPE Configuration Message and bend range again.
- **Audio out:** Windows: WASAPI in shared mode by default (the Windows mixer and volume stay in
  the path), one device period queued; following the default output. Exclusive mode on request
  (Settings > audio output): the device to awva alone, its smallest buffer (or 256, 512 frames), the
  first format it takes; awva then applies the Windows volume itself (the endpoint's level and mute,
  ramped, as the mixer would; left to the device where it does it in hardware), and a device whose
  volume can't be read plays shared. macOS: CoreAudio's default output, a 128-frame buffer.
- **The delay, key to sound:** shown part by part (Settings; the total in the footer), each figure
  measured or as the driver reports it: MIDI in to output (measured, min/mean/max: a message plays
  from the next buffer rendered, under one period from its arrival), the engine's 2 ms step and
  32-frame block, the output queued, the device's own latency. `awva_app_cli devices` asks the
  default output what periods it offers, without opening a stream.

## Safety

- The app starts at -18 dB, or at the level you left it at, but never louder than -12 dB, and fades
  in over 300 ms.
- A true-peak limiter at -1 dBTP is always on after the level (1.4 ms look-ahead, inter-sample peaks
  by 4x oversampling). Below its ceiling it passes the samples exactly.
- A hard ceiling at 0 dBFS after it.
- An engine whose output isn't finite, or passes awva's bound (8, before the app's x0.25), is muted
  and reset at once. A package's engine that does it is stopped until you choose another engine.
- The OS's volume applies after all of this.

## Your engines: packages (`.awvapkg`, awva-package/2)

An engine build ships as one file, `<id>-<slug>.awvapkg`. The same file is loaded by:
- this app (its PC image);
- the virtual board and the board (their own images, through the firmware's module store);
- Striso Live, which sends it to them unchanged.

The app reads them from:
- Windows: `%APPDATA%\awva\engines\`
- macOS: `~/Library/Application Support/awva/engines/`

**The container** (awva's `component/include/nsc/package.h`, packed by `component/tools/nsc_pack.py`):

| part | |
|---|---|
| header, 64 B | `'NSCP'`, format 1, the NSC_ABI, the schema hash, the section table's place, the size, CRCs |
| section table | 48 B per section: kind, tag, offset, size, crc32 |
| kind 1, manifest | JSON, below |
| kind 2, image | tag = target: `x86_64-windows`, `arm64-macos`, `x86_64-macos`, `x86_64-linux`, `armv7em-fpv5-d16-softfp`. A 256 B module header, then (Arm only) the relocations, then the image: on a PC the DLL / dylib / so as it is |
| kind 3, preset | tag = the file's stem (`01-soprano`), the `awva-preset/1` JSON |

**The manifest:**

```json
{"format": "awva-package/2", "label": "clarinet v3 (fit)", "id": "clarinet", "awva": "<commit>",
 "abi": 4, "images": ["x86_64-windows", "armv7em-fpv5-d16-softfp"], ...nsc_manifest's fields}
```

- `label`: the name the app lists it by. The engine's title and version come from its image.
- `awva`: the awva commit the images are built on. It must be this app's pinned commit
  (`AWVA_COMMIT`); a package built on any other is refused without loading it, as "built for a
  different awva".
- `images`: the targets in it. An empty list makes a **presets-only** package: new presets for the
  built-in engine `id`.

**What the app checks:**
- the header and section CRCs, format 1, the ABI;
- the pin;
- that an image for this computer is present.

A refused package is listed with its reason.

**What it loads:**
- the image, from a copy, so the package can be written again while it plays;
- the factory presets, then the package's preset sections in tag order. A preset of another engine
  is listed as refused.

Each package is its own entry, even if it has the same engine id as a built-in or another package.
A package is written as `<name>.awvapkg.tmp` and then renamed, so the app never sees half of one.

**Making one:** `app/tools/iterate.sh` (below) for an engine change. Or awva's packager directly:
`nsc_pack.py host <library> <manifest.json> <tag> <toolchain> <x.nscm>`, then
`nsc_pack.py package <x.awvapkg> --image <tag>=<x.nscm> --manifest <manifest.json> --label <text> --awva <commit>`.

**Checking one without the app or a device:**
- `build/app/awva_app_cli list` lists every engine and package, with why one is refused.
- `awva_app_cli check --engine package:<name> [--against builtin:<id>]` renders every preset
  offline through the app's own path, and compares them with another engine.

## The iteration loop

`app/tools/iterate.sh ENGINE AWVA_DIR [--presets-only] [--label TEXT]`:
1. It builds an engine change into `<engine>-iter<n>.awvapkg`. The change is an awva checkout on
   the app's awva commit; its nsc.h is checked against the pin. The package is built with awva's
   own sdk and packager.
2. It checks the package as the app plays it (`awva_app_cli check`: every preset finite and
   sounding), and compares it with the built-in engine, preset by preset.

The running app lists the package within two seconds, because it scans the folder again whenever
it changes. Ctrl+R / Cmd+R, or "Rescan your engines", rescans at once.

`--presets-only` packs `presets/<engine>/` as a presets-only package, with no build. An unchanged
checkout gives a package that plays bit for bit as the built-in engine.

## Running

```
awva [--no-audio] [--no-midi] [--hidden] [--engines-dir DIR] [--probe-wav FILE] [--exit-after S]
```

- `--no-audio`: no audio device; the core's null sink asks for audio in real time instead.
- `--no-midi`: no MIDI port is opened.
- `--hidden`: no window (and no Dock icon on macOS).
- `--probe-wav FILE`: every output sample, after the safety chain, also written to FILE (32-bit float).
- `--exit-after S`: quit after S seconds.

Smoke runs on someone's machine use `--no-audio --no-midi --hidden`.

## Building

The core first, with the plugins (the repository's root):

```
./fetch_deps.sh
./build.sh build                       # build/app: the core, the built-in engines, awva_app_cli
ctest --test-dir build -L app          # the core's checks, a smoke run, the app against each plugin
```

Then the app (Flutter 3.41), which takes the core from `build/app`:

```
cd app
flutter test                           # widgets, the screenshots (test/goldens), the core through FFI
flutter build windows                  # build/windows/x64/runner/Release/
flutter build macos                    # build/macos/Build/Products/Release/awva.app (universal)
```

`tool/make_fixtures.dart` writes the screenshots' data (`test/fixtures`) from the real core.

## Tests

- `ctest -L app` (C++):
  - `app.core`: the safety chain (bit-transparent below the ceiling, the true-peak ceiling, the
    fault mute, the start), the packages (each rule above), the C ABI, an MPE setup surviving an
    engine switch, MIDI 1.0 to UMP;
  - `app.smoke`: the live path (MIDI rings, timestamps, the null sink, the probe tap) with an MPE
    controller's stream sent in real time;
  - `app.<engine>.equals_plugin`: an MPE controller's stream (MIDI 1.0) and a MIDI 2.0 stream, the
    default sound and a factory preset, 48 and 44.1 kHz, host blocks of 512 and random sizes: the
    app's engine output equals the engine's CLAP plugin's, bit for bit, and so does the whole
    output through the safety chain at 0 dB wherever the plugin stays under the limiter's ceiling.
- `flutter test` (Dart): the screen against a fixed core, golden screenshots in both themes, and
  `test/ffi_test.dart` against the real core library (null sink, no MIDI).
