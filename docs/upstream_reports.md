# Upstream bug reports (drafts)

These are drafts only. None has been filed: whether to file them is the user's decision. Each one was found while building and validating the awva plugins.

| # | project | version | what |
|---|---|---|---|
| 1 | clap-validator | 0.4.1 (152b982) | the preset-discovery-crawl and -load tests deadlock |
| 2 | clap-validator | 0.4.1 (152b982) | process-varying-sample-rates fails a plugin that refuses a sample rate |
| 3 | clap-validator | 0.4.1 (152b982) | the param-set-events tests can fail when every parameter is stepped |
| 4 | clap-wrapper | 0.17.0 (806bba0) | an AUv2's Info.plist is lost when CMake configures again |
| 5 | clap-wrapper | 0.17.0 (806bba0) | `target_add_vst3_wrapper(... ASSET_OUTPUT_DIRECTORY)` is ignored on macOS and for a single-file Windows VST3 |
| 6 | clap-wrapper | 0.17.0 (806bba0) | `target_add_auv2_wrapper` fails for an OUTPUT_NAME with spaces |
| 7 | clap-wrapper | 0.17.0 (806bba0) | the AUv2 still returns a Cocoa view for a plugin without the gui extension |
| 8 | clap-wrapper | 0.17.0 (806bba0) | the AUv2's class data doesn't keep the current preset's name |

---

## 1. clap-validator: preset-discovery-crawl and preset-discovery-load deadlock

**Version:** clap-validator 0.4.1 (152b982). Seen with the release binaries on Windows and on macOS, arm64.

**Repro:**
1. Write a CLAP whose preset-discovery provider declares one location of kind `CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN` (location `NULL`).
2. In `get_metadata()`, list two presets, each with a load key:
   ```c
   r->begin_preset(r, "First", "0");
   r->begin_preset(r, "Second", "1");
   ```
3. Run `clap-validator validate plugin.clap`.

**Expected:** both presets are indexed, and the two tests pass or fail on their merits.

**Actual:** both tests hang forever, and the run never finishes. Excluding them (`-x 'preset-discovery-(crawl|load)'`) lets the rest run.

**Cause:** in `src/plugin/preset_discovery/metadata_receiver.rs`, `begin_preset()` takes `this.result.lock()` and keeps the guard alive. With a load key it then calls `this.flush_preset()`, which locks the same `Mutex` again, so the thread deadlocks on itself.

**Fix:** drop the guard before calling `flush_preset()`, for example by checking the result in its own scope.

---

## 2. clap-validator: process-varying-sample-rates fails a plugin that refuses a sample rate

**Version:** clap-validator 0.4.1 (152b982).

**Repro:**
1. Write a CLAP whose `activate()` returns `false` for sample rates outside the range its DSP supports, for example 11025 to 192000 Hz.
2. Run `clap-validator validate plugin.clap -t process-varying-sample-rates`.

**Expected:** the CLAP spec lets `activate()` fail. So a rate the plugin refuses should be skipped, or reported as a warning, and the test should check the output at the rates the plugin does accept. It could fail only if the plugin accepts none of them.

**Actual:** the test fails at the first rate the plugin refuses, with "Error while processing with 8000.00hz sample rate: 'clap_plugin::activate()' returned false". It tries 8000, 22050, 44100, 48000, 88200, 96000, 192000, 384000, 768000, 1234.5678, 12345.678, 45678.901 and 123456.78 Hz, so a plugin with any lower or upper limit fails.

---

## 3. clap-validator: param-set-events and param-set-no-cookies can fail when every parameter is stepped

**Version:** clap-validator 0.4.1 (152b982).

**Repro:**
1. Write a CLAP whose only parameter is a stepped toggle: `CLAP_PARAM_IS_STEPPED`, range 0..1, default 0.
2. Run `clap-validator validate plugin.clap -t param-set`.

**Expected:** the test sets values that differ from the current ones, or skips when it can't, and passes when the plugin takes them.

**Actual:** "After calling 'clap_plugin_params::flush()', the parameter values did not change". The test draws one random value per parameter with a fixed seed (`ParamFuzzer::randomize_params_at`, `random_value()` rounds stepped values). For a single toggle it can draw the current value, 0. The plugin's values then really don't change, and the check `flush_param_values == initial_param_values` fails.

---

## 4. clap-wrapper: an AUv2's Info.plist is lost when CMake configures again

**Version:** clap-wrapper 0.17.0 (806bba0). Also seen with 0.9.1. CMake 3.29.2, Ninja 1.12.0, macOS 26.

**Repro:**
1. Build an AUv2 with `target_add_auv2_wrapper(TARGET foo_auv2 ...)`. Its `Contents/Info.plist` has the `AudioComponents` entry, and auval finds the component.
2. Run the CMake configure step again, for example `cmake -S . -B build`.
3. Build again with no source changes: `ninja -C build`, or `ninja -C build foo_auv2`.

**Expected:** the bundle's Info.plist still has `AudioComponents`.

**Actual:** after step 2 the Info.plist is CMake's generic bundle plist (no `AudioComponents`, `CFBundlePackageType` APPL), and step 3 doesn't restore it. auval no longer finds the component.

**Cause:**
- CMake writes a bundle target's Info.plist at generate time.
- `wrap_auv2.cmake` copies the build helper's `auv2_Info.plist` into the bundle as a `PRE_BUILD` custom command of the AU target (line 283).
- With Ninja and Makefiles, `PRE_BUILD` runs only when the target links.

**Workaround:** a custom target that always copies the plist and re-signs the bundle.

---

## 5. clap-wrapper: `target_add_vst3_wrapper(... ASSET_OUTPUT_DIRECTORY ...)` is silently ignored on macOS and for single-file Windows VST3s

**Version:** clap-wrapper 0.17.0 (806bba0).

**Repro:** on macOS, call `target_add_vst3_wrapper(TARGET foo_vst3 OUTPUT_NAME foo ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")` and build.

**Expected:** `foo.vst3` is written to `${CMAKE_BINARY_DIR}/out`.

**Actual:** the option has no effect, and there is no error or warning.

**Cause:** `cmake/wrap_vst3.cmake` lines 168 (macOS) and 212 (the Windows single-file case) call `set_target_properties(${VST3_TARGET} ...)`, but the target variable is `V3_TARGET`. `VST3_TARGET` is unset, so `set_target_properties()` gets no target and does nothing. The Windows folder-bundle branch (around line 236) uses `V3_TARGET` correctly.

---

## 6. clap-wrapper: `target_add_auv2_wrapper` fails for an OUTPUT_NAME with spaces

**Version:** clap-wrapper 0.17.0 (806bba0). Also seen with 0.9.1.

**Repro:** `target_add_auv2_wrapper(TARGET foo_auv2 OUTPUT_NAME "My Synth" ...)`.

**Expected:** a `My Synth.component`. The VST3 wrapper accepts the same name.

**Actual:** the configure step fails with "string sub-command MAKE_C_IDENTIFIER requires two arguments".

**Cause:** `cmake/wrap_auv2.cmake` line 198 calls `string(MAKE_C_IDENTIFIER ${AUV2_OUTPUT_NAME} outidentifier)` without quotes, so the name splits into two arguments. Quoting `"${AUV2_OUTPUT_NAME}"` fixes this line; the other unquoted uses of the name may need the same.

---

## 7. clap-wrapper: the AUv2 still returns a Cocoa view for a plugin without the gui extension

**Version:** clap-wrapper 0.17.0 (806bba0), `src/wrapasauv2.cpp`.

This is from reading the code; I didn't reproduce it at runtime. 0.17.0 fixed the crash I saw with 0.9.1: auval dereferenced a null gui extension at "VERIFYING CUSTOM UI". `GetPropertyInfo(kAudioUnitProperty_CocoaUI)` now returns `kAudioUnitErr_InvalidProperty` without a gui (line 1022). One path is left:
- `GetProperty(kAudioUnitProperty_CocoaUI)` without a gui extension, or without Cocoa support, takes the "Mysterious" branch (lines 1154-1158).
- That branch calls `fillAudioUnitCocoaView()` and returns `noErr`.

**Expected:** `kAudioUnitErr_InvalidProperty`, as `GetPropertyInfo` returns, so a host that calls `GetProperty` directly gets no view for a plugin without a GUI.

**Actual (by reading):** the host gets a view class for a plugin that has no GUI.

---

## 8. clap-wrapper: the AUv2's class data doesn't keep the current preset's name

**Version:** clap-wrapper 0.17.0 (806bba0), `src/wrapasauv2.cpp`; AudioUnitSDK 1.1.0.

**Repro:**
1. Wrap any CLAP that has the state extension as an AUv2.
2. Run `auval -strict -v aumu <subtype> <manufacturer>`.

**Expected:** auval sets a user preset with a name (`kAudioUnitProperty_PresentPreset`, `presetNumber` -1), and the class data it then reads (`kAudioUnitProperty_ClassInfo`) carries that name under `kAUPresetNameKey`. The same holds for a factory preset chosen through `PresentPreset`.

**Actual:** "WARNING: Preset name is not retained in retrieved class data", on every AU.

**Cause:**
- `WrapAsAUV2::SaveState()` (around line 2256) writes `_current_program_name` as the preset name. That is "Program", or the name of the last class data restored (`RestoreState`).
- The preset the host set is kept by the AU SDK as `mCurrentPreset` (`AUBase::NewCustomPresetSet` / `SetAFactoryPresetAsCurrent`), and `AUBase::SaveState` writes that one.

**Fix:** write `mCurrentPreset.presetName` (`AUBase::GetPresentPreset`) when it is set, else `_current_program_name`.
