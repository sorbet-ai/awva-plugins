<p align="center"><img src="docs/logo/awva.svg" alt="awva" width="480"></p>

# awva-plugins

[awva](https://github.com/sorbet-ai/awva) is a set of portable, expressive per-note sound engines. The same engine code runs on PCs and on embedded devices, and is played by MIDI 2.0 and MPE. awva-plugins brings the engines to DAWs: every engine as a CLAP, a VST3 and an Audio Unit (AUv2), also played by CLAP note expressions.

- **Formats:**
  - CLAP on Windows and macOS;
  - VST3 on Windows and macOS;
  - AUv2 on macOS.
- **Platforms:** Windows x64, and macOS 11 or later, universal (arm64 and x86_64).
- **Input:** per-note expression in every format, by each format's own events (see [Expressive input](#expressive-input)):
  - CLAP: notes with per-note pressure, tuning and brightness expressions; MIDI 2.0 (UMP, the preferred dialect); MIDI 1.0 and MPE;
  - VST3: notes with poly pressure and the tuning and brightness note expressions; MPE through the MIDI controllers of all 16 channels;
  - AU: MIDI 2.0 (the AU asks its host for it); MIDI 1.0 and MPE, which the system hands the AU as MIDI 2.0.

  A note plays on its own MIDI channel, with its own id, so any number of notes can overlap. The engine's voice allocator takes over the oldest voice when it runs out. Each CLAP note gets one NOTE_END, when the engine hands the note back.
- **Output:** stereo; a mono engine is on both channels.
- **Editor:** the awva app's screen, drawn natively (Direct2D and DirectWrite on Windows, Core
  Graphics and Core Text on macOS) to `app/DESIGN.md`: the presets, the hero knobs, every
  parameter, legato, light and dark. Its changes reach the host as CLAP gestures and values (so a
  DAW records them as automation), through the VST3's and the AU's views as well (clap-wrapper).
  The design tokens are generated with the app's from one file (`app/design/tokens.json` ->
  `include/awva_plugin/design_tokens.h`); the font (DM Sans, OFL) and the logo are in the binary,
  and nothing is installed. The theme follows the OS unless chosen in the editor, for every awva
  plugin (`<app data>/awva/plugin-editor.json`). `ctest -R editor_shot` renders it offscreen in both
  themes (`build/editor_shots/`).
- **Presets:** each engine's factory presets, in their order, in every format:
  - CLAP: preset-discovery, with the factory presets as the plugin's location, and preset-load;
  - VST3: the plugin's preset list, the program list of its root unit;
  - AU: its factory presets.

  A preset file (`awva-preset/1`, `.preset.json`, as in awva's `presets/`) loads through CLAP preset-load from its path.
- **State:** a project saves the sound as an `awva-preset/1` file, and a project restores it exactly. It also stores the output level, legato and the factory preset the sound started from (see below).

All the plugins come from one generic source, `src/plugin.cpp`, which sees only CLAP and awva's engine component ABI (`nsc/nsc.h`, ABI 2). Each engine's component is linked in, built by awva's `awva_add_component(<target> <engine> STATIC)`. The plugin takes everything it shows from the component's descriptor: the name, summary, parameters, factory presets, voices and tail. clap-wrapper makes the VST3 and the AU from the same code.

## The standalone app

`app/` is awva as an instrument of its own, for Windows and macOS: an engine and preset picker, the
hero knobs, MIDI 2.0, MPE and MIDI 1.0 in, and audio out behind a safety chain. Its core is C++
built here with the plugins (so it plays what they play: `ctest -L app` checks each engine's output
against its CLAP's, bit for bit), and its UI is Flutter. It also plays engine packages from a
folder, so new builds of an engine can be played without rebuilding anything. See
[app/README.md](app/README.md) and [app/DESIGN.md](app/DESIGN.md).

## The engines

| engine | name | what it is | parameters | factory presets | voices | AU subtype |
|---|---|---|---|---|---|---|
| accordion | Accordion | free-reed accordion: registers, musette tuning, reed chambers, cassotto, a shared bellows | 43 | 9 | 10 | Acco |
| bowed | Bowed Strings | bowed strings: friction-driven waveguide strings, modal body, section | 39 | 9 | 10 | Bowe |
| brass | Brass | brass: a lip source and a physical bore, bell, mutes | 44 | 9 | 10 | Bras |
| ep | Electric Piano | electric pianos: modal tines and reeds, pickups, damper | 26 | 14 | 10 | Ep__ |
| modal | Modal | tuned percussion: banks of damped resonators, struck or rubbed | 21 | 6 | 10 | Moda |
| reed | Reed | single reed: clarinet to saxophone, one model, tilt morphs the bore | 1 | 0 | 10 | Reed |
| sine | Sine | the minimal example engine: one sine per voice, level from pressure | 1 | 0 | 10 | Sine |
| trumpet | Trumpet | physical trumpet: lips, mouthpiece and bore | 46 | 1 | 10 | Trum |
| va | Virtual Analog | virtual analog: saw/pulse/sub oscillators, sync, ladder-style filter, envelopes | 56 | 8 | 10 | Va__ |
| wind | Wind | woodwinds: reed and jet driven bores, cylindrical and conical | 37 | 9 | 10 | Wind |

The parameters are the engine's, plus the component's `output` level (0 to 2, default 1). Every plugin also has a `legato` parameter, which switches the engine to mono legato.

The name is the engine's title (`nsc_desc.name`); the id is its short name (`nsc_desc.id`).

**Identity:**
- The plugin id is `com.sorbet-ai.awva.<engine>`, and the vendor is "Sorbet AI".
- The files are `awva_<engine>.clap`, `.vst3` and `.component`. On Windows the `.vst3` is the VST3 folder bundle (`Contents/x86_64-win/`).
- The AU type is `aumu` and its manufacturer code is `Awva`.
- The subtype is the engine id's first four letters, the first capitalised, padded with `_`. The build stops if two engines would share a subtype.

## Presets and state

**Factory presets.** The presets come from the component's descriptor, so each plugin lists exactly its engine's factory presets, by name and in their order. clap-wrapper builds the VST3's and the AU's lists from the plugin's own preset discovery.

**The state.** A host's save and restore of the plugin use the sound as an `awva-preset/1` JSON file, as the component's `save_preset()` writes it: the engine, the title, the heroes, and every parameter by name. One more key holds the plugin's own settings, which a preset reader skips:

```json
"awva-plugin": {"output": 1, "legato": false, "factory_preset": 3}
```

- `output` is the component's output level. It is not part of a preset.
- `legato` is mono legato.
- `factory_preset` is the factory preset the sound started from, or -1 for none.

**Restoring.**
- **A sound made from a factory preset** is restored the way it was made: the preset by its index, then the values that differ from it. The component has that preset as its own again, with its heroes and `preset()`, and a sound edited after a preset comes back bit for bit.
- **Any other sound** is read by the component's `load_preset()`.

The info proof checks that a restored project gives the same sound, bit for bit, and saves the same state text again. It does this on every engine, for four cases:
- the defaults;
- random parameters;
- a factory preset;
- a factory preset followed by random parameters.

A state from another engine, or text that isn't a preset, is refused.

## Building

You need CMake 3.25 or later, Ninja, Python 3, git and, for the validators, Rust 1.95 or later, plus one of:
- **Windows:** Visual Studio 2022 (its libraries) and LLVM clang.
- **macOS:** Xcode and Homebrew's cmake and ninja.

```
git clone --recurse-submodules https://github.com/sorbet-ai/awva-plugins.git
./fetch_deps.sh --validators
./build.sh all          # configure, build, and run the proofs (ctest -L plugin)
tools/validate.sh       # clap-validator, pluginval 10, and on macOS auval -strict
tools/package.sh windows-x64      # or macos-universal: dist/awva-plugins-<version>-<platform>.zip
```

**Dependencies:** `fetch_deps.sh` puts each one in its own directory under `deps/`. Each is checked against a pinned commit or SHA-256. awva itself is pinned by `AWVA_COMMIT`: a change in awva reaches the plugins only through a bump of that file. clap-wrapper and clap-validator are git submodules under `third_party/`, pinned to commits of Sorbet AI's forks ([docs/forked_dependencies.md](docs/forked_dependencies.md)). `fetch_deps.sh` checks them out and builds clap-validator from its source.

**Proofs:** `ctest -L plugin` runs these on each engine.
- **info:** the plugin matches its component: the same id, name, parameters and presets. It also checks:
  - the ports;
  - preset loading, by index and from each of awva's preset files, which give their factory preset's values bit for bit;
  - a state round trip, both active and inactive;
  - that a restored project is exact, in the four cases above.
- **conformance:** awva's `awva_conformance`, the component ABI's conformance checks, one ctest per check (`plugin.<engine>.conformance.<check>`) (awva's `docs/conformance.md`), on the engine's component as the plugins build it: the same `awva_add_component`, here built as a loadable library. A component change that breaks the ABI contract fails this repository's CI too. The engines' known failures are awva's own (`sdk/tests/known_failures.cmake`), and they are reported as warnings, as they are in awva.
- **vst3_presets:** a minimal VST3 host (`tests/vst3_host_test.cpp`, the VST3 SDK's hosting module) reads the VST3's preset list. It must hold the factory presets, by name and in order.
- **expressive.clap, .vst3, .au:** the same expressive performances through each format's own events, against awva_render's render of their UMP stream, bit for bit (see [Expressive input](#expressive-input)).
- **perf:** the engine's default preset playing the awva SDK's default scripted performance.
  - The proof runs it through the plugin by a private test extension, `com.sorbet-ai.awva.voice-controls/1`; a DAW never uses it.
  - The output must equal `awva_render --ftz`'s x 0.25, bit for bit, at host blocks of 256, 1, 33 and random sizes. `--ftz` runs each block with subnormals flushed, as the component does.
- **midi:** a Standard MIDI File (MPE notes with bends, pressure and CC74, a master-channel note, the pedal) in the MIDI dialect, each event at its time rounded to the nearest frame. The output must equal `awva_render --ftz --midi`'s x 0.25, bit for bit, at 48 and 44.1 kHz and host blocks of 256, 1, 33 and random sizes. The plugin and the renderer take MIDI by the same integer-frame step rule.
- **ump:** MIDI 2.0 and MIDI 1.0 input, polyphonic and legato, at 48 and 44.1 kHz and host blocks of 256, 1, 33 and random sizes. The output must equal, bit for bit, the engine's own component library given the same UMP at the same frames.
- **notes:** CLAP notes and expressions must give the same output as their MIDI 2.0 translation, whatever the host block size, with a NOTE_END for each note.
- **many:** 20 overlapping CLAP notes, each with its own brightness:
  - every note sounds: without it, the output differs;
  - every note's brightness is its own: changing it changes the output (checked for the engines that read y);
  - each note gets exactly one NOTE_END, with its id.

None of the proofs or validators opens an audio device.

## Expressive input

The proof `plugin.<engine>.expressive.<format>` plays the same two performances (`tests/expressive.h`) through each format's own events. The output must equal, bit for bit and x 0.25, `awva_render --ftz --ump`'s render of the UMP stream that the format carries to the engine. It runs at 48 and 44.1 kHz, in host blocks of 128 (CLAP: 256), 1, 33 and random sizes.
- **per-note:** five overlapping notes on three channels, two of them on the same key. Each has its own pressure, tuning and brightness, and every value is exact in every format.
- **MPE:** the bend range set by RPN, five notes on member channels, each channel's pitch bend, pressure and CC74, and the master channel's bend.

Each check also confirms that the per-note pressure, tuning and brightness each change the output. Brightness is checked for the engines that read timbre.

| format | per-note | MPE | the UMP the engine gets |
|---|---|---|---|
| CLAP note expressions | notes; pressure, tuning, brightness expressions | (MIDI dialect) | MIDI 2.0 Note On/Off (16-bit velocity), Poly Pressure, Registered Per-Note Controller 3 (Pitch 7.25), Assignable Per-Note Controller 4 |
| CLAP MIDI2 (UMP) | the UMP as it is | the UMP as it is | the same |
| CLAP MIDI / MIDI_MPE | (MPE) | MIDI 1.0 bytes | MIDI 1.0 in UMP |
| VST3 | notes; poly pressure events; tuning and brightness note expressions | notes; each channel's pitch bend, channel pressure and CC74 as its MIDI controllers' parameter changes (IMidiMapping) | per-note as CLAP's; MPE: notes as MIDI 2.0, controllers as MIDI 1.0 |
| AU | MIDI 2.0 event lists | MIDI 1.0 (MusicDeviceMIDIEvent, or MIDI 1.0 event lists) | per-note as it is; MPE as the UMP spec's default MIDI 1.0 to 2.0 translation |

**What a format can't carry exactly:**
- **No format lacks a per-note dimension.** Each one carries per-note pressure, pitch and timbre: through note expressions (CLAP, VST3), MIDI 2.0 per-note messages (CLAP, AU), or MPE channels (every format).
- **VST3 MPE velocities:** VST3 notes carry 32-bit float velocities, so the engine gets an MPE note's velocity as MIDI 2.0's 16 bits, not MIDI 1.0's 7. The physical models are chaotic, so this tiny change gives a different waveform: up to -65 dBFS from the MIDI 1.0 path (brass), -72 dBFS for bowed, and -98 to -106 dBFS for the others. It is not a different sound.
- **VST3 MIDI controllers:** clap-wrapper turns a host block's parameter changes into MIDI by their last point. So a channel's bend, pressure or CC74 changes at most once per host block, at that point's frame.
- **VST3 per-note values** are float32 (pressure) or double (note expressions). The proofs' values are exact in both; a host's arbitrary value is rounded to float32 before the 32-bit MIDI 2.0 word.
- **AU MIDI 1.0:** the AU asks for MIDI 2.0, so the system translates MIDI 1.0 (min-center-max scaling, the RPN as a Registered Controller). The engine then hears MPE at MIDI 2.0's resolution, not as the MIDI 1.0 bytes.

## Performance

CPU load as a % of one core, at 48 kHz in host blocks of 128. The workload is 1 / 4 / 10 notes held, each with its pressure, tuning and brightness moving every 10 ms, and each figure is the best of 5 runs of 4 s. "component" is the bare engine component given the same UMP; the formats' extra cost over it is the wrapper's.

BENCH_TABLES

**Latency:** every format reports 0 samples, since the plugin never delays its output. A Note On sounds at the engine's next 2 ms step, so 0 to 91 frames at 48 kHz, plus the engine's own onset (32 for modal, 64 for ep, about 150 for brass and trumpet). This is the same in every format.

`./build.sh bench` makes the tables (`build/bench/table.md`). CI runs it on Windows and macOS as a regression check: every format must stay under 25 % of a core at 10 voices, and at most twice the bare component's load plus 0.5 %.

## Validators

The last run gave the same result on Windows and macOS:

| engine | clap-validator (fork of 0.4.1), every test | pluginval 1.0.4, strictness 10 (VST3) | auval -strict (AU) |
|---|---|---|---|
| all 10 engines | pass | pass | pass |

clap-validator runs all its tests, the preset-discovery ones included. It is built from Sorbet AI's fork, and clap-wrapper is a fork too: [docs/forked_dependencies.md](docs/forked_dependencies.md) lists their changes. The plugins refuse sample rates outside the engines' own range (`min_rate` .. `max_rate`, 11.025 to 192 kHz), as CLAP allows; the validator skips those rates, and the info proof checks the range.

## Timing

The engines take their input in 2 ms steps on an integer frame grid and render 32-frame blocks. A host block can end inside a 32-frame block. An event in the rest of that block then arrives after the block was rendered.
- The plugin gives such an event to the next block as a late event, with a negative frame (`nsc_event.frame`, -31 .. -1).
- The engine takes it at the step its own frame is due at.
- So any host block size gives the same samples at any sample rate. The midi and ump proofs check this at 44.1 and 48 kHz.

The supported sample rates are the engines': 11.025 to 192 kHz (`nsc_desc.min_rate` .. `max_rate`).

## Signing

The CI workflow (`.github/workflows/ci.yml`) has steps for:
- macOS code signing, hardened runtime, notarization and stapling;
- Windows Authenticode signing.

They are off: `SIGNING: "false"` in the workflow. To turn them on, add these secrets and set `SIGNING` to `"true"`:
- **macOS:** `MACOS_CERTIFICATE_P12_BASE64`, `MACOS_CERTIFICATE_PASSWORD`, `MACOS_SIGNING_IDENTITY`, `MACOS_NOTARY_APPLE_ID`, `MACOS_NOTARY_TEAM_ID`, `MACOS_NOTARY_PASSWORD`.
- **Windows:** `WINDOWS_CERTIFICATE_PFX_BASE64`, `WINDOWS_CERTIFICATE_PASSWORD`.

Until then, the macOS bundles are signed ad hoc, and macOS will quarantine them when they are downloaded.


## Releases

A tag `v<version>` must match `project(... VERSION ...)` in `CMakeLists.txt`. It publishes a GitHub release with:
- `awva-plugins-<version>-windows-x64.zip` (CLAP, VST3);
- `awva-plugins-<version>-macos-universal.zip` (CLAP, VST3, AU);
- `SHA256SUMS`.

Each archive holds the licence texts in `licenses/`.

## Licensing

- **The source here:** Apache-2.0 (`LICENSE`), Copyright 2026 Sorbet AI. Every file carries an SPDX header, and `NOTICE` lists what the binaries include.
- **The binaries:** every dependency they are built from is under a permissive licence (Apache-2.0, MIT, BSD-3-Clause or public domain), so none of the CLAP, VST3 or AU binaries contains GPL code.
- **VST3 binaries:** they are built with the VST3 SDK 3.8.0, which Steinberg released under the MIT licence in October 2025. That covers the whole SDK, including the parts clap-wrapper compiles (`base`, `pluginterfaces`, `public.sdk`).

| dependency | version | used in | licence |
|---|---|---|---|
| awva (engines, component, SDK) | `AWVA_COMMIT` | every binary | Apache-2.0; includes fastapprox's fastpow2 (BSD-3-Clause) and Faust fast.lib coefficients (Apache-2.0) |
| CLAP SDK | 1.2.10 | every binary (headers) | MIT |
| clap-wrapper (with fmt and the PreSonus extensions header) | Sorbet AI's fork of 0.17.0 (`third_party/clap-wrapper`) | VST3, AU | MIT (fmt: MIT; PreSonus header: public domain) |
| VST3 SDK (base, pluginterfaces, public.sdk) | 3.8.0 (build 66) | VST3 | MIT |
| AudioUnitSDK | 1.1.0 | AU | Apache-2.0 |
| doctest (via awva) | awva's | tests only | MIT |
| clap-validator | Sorbet AI's fork of 0.4.1 (`third_party/clap-validator`) | CI only, not shipped | MIT |
| pluginval | 1.0.4 | CI only, not shipped | GPLv3 |

VST is a trademark of Steinberg Media Technologies GmbH.
