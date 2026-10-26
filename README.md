<p align="center"><img src="docs/logo/awva.svg" alt="awva" width="480"></p>

# awva-plugins

[awva](https://github.com/sorbet-ai/awva) is a set of portable, expressive per-note sound engines. The same engine code runs on PCs and on embedded devices, and is played by MIDI 2.0 and MPE. awva-plugins brings the engines to DAWs: every engine as a CLAP, a VST3 and an Audio Unit (AUv2), also played by CLAP note expressions.

- **Formats:**
  - CLAP on Windows and macOS;
  - VST3 on Windows and macOS;
  - AUv2 on macOS.
- **Platforms:** Windows x64, and macOS 11 or later, universal (arm64 and x86_64).
- **Input:**
  - CLAP notes with per-note pressure, tuning and brightness. All of them go to the engine on one MIDI 2.0 channel, each with its own id, so any number of notes can overlap. The engine's voice allocator takes over the oldest voice when it runs out. Each note gets one NOTE_END, when the engine hands the note back;
  - MIDI 1.0 and MPE;
  - MIDI 2.0 (UMP).
- **Output:** stereo; a mono engine is on both channels. There is no GUI: the host shows the parameters.
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

You need CMake 3.25 or later, Ninja, Python 3 and git, plus one of:
- **Windows:** Visual Studio 2022 (its libraries) and LLVM clang.
- **macOS:** Xcode and Homebrew's cmake and ninja.

```
./fetch_deps.sh --validators
./build.sh all          # configure, build, and run the proofs (ctest -L plugin)
tools/validate.sh       # clap-validator, pluginval 10, and on macOS auval -strict
tools/package.sh windows-x64      # or macos-universal: dist/awva-plugins-<version>-<platform>.zip
```

**Dependencies:** `fetch_deps.sh` puts each one in its own directory under `deps/`. Each is checked against a pinned commit or SHA-256. awva itself is pinned by `AWVA_COMMIT`: a change in awva reaches the plugins only through a bump of that file.

**Proofs:** `ctest -L plugin` runs eight of them on each engine.
- **info:** the plugin matches its component: the same id, name, parameters and presets. It also checks:
  - the ports;
  - preset loading, by index and from each of awva's preset files, which give their factory preset's values bit for bit;
  - a state round trip, both active and inactive;
  - that a restored project is exact, in the four cases above.
- **conformance:** awva's `awva_conformance`, the component ABI's conformance checks, one ctest per check (`plugin.<engine>.conformance.<check>`) (awva's `docs/conformance.md`), on the engine's component as the plugins build it: the same `awva_add_component`, here built as a loadable library. A component change that breaks the ABI contract fails this repository's CI too. The engines' known failures are awva's own (`sdk/tests/known_failures.cmake`), and they are reported as warnings, as they are in awva.
- **vst3_presets:** a minimal VST3 host (the VST3 SDK's hosting module) reads the VST3's preset list. It must hold the factory presets, by name and in order.
- **perf:** the engine's default preset playing the awva SDK's default scripted performance.
  - The proof runs it through the plugin by a private test extension, `com.sorbet-ai.awva.voice-controls/1`; a DAW never uses it.
  - The output must equal `awva_render --ftz`'s × 0.25, bit for bit, at host blocks of 256, 1, 33 and random sizes. `--ftz` runs each block with subnormals flushed, as the component does.
- **midi:** a Standard MIDI File (MPE notes with bends, pressure and CC74, a master-channel note, the pedal) in the MIDI dialect, each event at its time rounded to the nearest frame. The output must equal `awva_render --ftz --midi`'s × 0.25, bit for bit, at 48 and 44.1 kHz and host blocks of 256, 1, 33 and random sizes. The plugin and the renderer take MIDI by the same integer-frame step rule.
- **ump:** MIDI 2.0 and MIDI 1.0 input, polyphonic and legato, at 48 and 44.1 kHz and host blocks of 256, 1, 33 and random sizes. The output must equal, bit for bit, the engine's own component library given the same UMP at the same frames.
- **notes:** CLAP notes and expressions must give the same output as their MIDI 2.0 translation, whatever the host block size, with a NOTE_END for each note.
- **many:** 20 overlapping CLAP notes, each with its own brightness:
  - every note sounds: without it, the output differs;
  - every note's brightness is its own: changing it changes the output (checked for the engines that read y);
  - each note gets exactly one NOTE_END, with its id.

None of the proofs or validators opens an audio device.

## Validators

The last run gave the same result on Windows and macOS:

| engine | clap-validator 0.4.1 | pluginval 1.0.4, strictness 10 (VST3) | auval -strict (AU) |
|---|---|---|---|
| all 10 engines | pass | pass | pass |

**Known validator artefacts:** `tools/validate.sh` excludes these tests and says why.
- **clap-validator `preset-discovery-crawl` and `-load`:** they deadlock inside clap-validator 0.4.1 itself. `begin_preset` holds its result mutex and `flush_preset()` locks it again. The plugins' preset discovery is tested by the info proof instead.
- **clap-validator `process-varying-sample-rates`:** it activates the plugin at rates from 1 kHz to 768 kHz and fails at the first rate the plugin refuses. The plugins run only the engines' own rates (`min_rate` .. `max_rate`, 11.025 to 192 kHz), and `activate()` refuses others, as CLAP allows. The info proof checks that range.
- **auval warning:** every AU passes with one warning, "Preset name is not retained in retrieved class data".
  - auval sets a preset name through `kAudioUnitProperty_PresentPreset`, then reads the class data back.
  - clap-wrapper 0.17.0's `SaveState` writes its own `_current_program_name` ("Program", or the name of the last state restored), not the preset the AU SDK holds as current (`mCurrentPreset`, which `AUBase::SaveState` would write).
  - The plugin can't reach that code, so the fix belongs in clap-wrapper. It is drafted in `docs/upstream_reports.md`.

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
| clap-wrapper (with fmt and the PreSonus extensions header) | 0.17.0 | VST3, AU | MIT (fmt: MIT; PreSonus header: public domain) |
| VST3 SDK (base, pluginterfaces, public.sdk) | 3.8.0 (build 66) | VST3 | MIT |
| AudioUnitSDK | 1.1.0 | AU | Apache-2.0 |
| doctest (via awva) | awva's | tests only | MIT |
| clap-validator | 0.4.1 | CI only, not shipped | MIT |
| pluginval | 1.0.4 | CI only, not shipped | GPLv3 |

VST is a trademark of Steinberg Media Technologies GmbH.
