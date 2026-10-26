# Forked dependencies and their changes

Two dependencies are Sorbet AI's forks, with fixes for bugs found while building and validating the awva plugins. Each is a git submodule under `third_party/`, pinned to a commit of its fork's `awva` branch. That branch is upstream's exact version plus the fixes, each with a test. The licences (MIT) and the authors are upstream's.

| dependency | fork | upstream base | pinned commit |
|---|---|---|---|
| clap-wrapper | [sorbet-ai/clap-wrapper](https://github.com/sorbet-ai/clap-wrapper/tree/awva) | 0.17.0 (`806bba0`) | [`1dd4219`](https://github.com/sorbet-ai/clap-wrapper/commit/1dd42194a7a1581702d5b5b374b2415599de6f3a) |
| clap-validator | [sorbet-ai/clap-validator](https://github.com/sorbet-ai/clap-validator/tree/awva) | 0.4.1 (`152b982`) | [`6d72c15`](https://github.com/sorbet-ai/clap-validator/commit/6d72c15564eaf4de253f6b33337059f77358a08d) |

## clap-wrapper

In [`5a8ac81`](https://github.com/sorbet-ai/clap-wrapper/commit/5a8ac813c73114acd02115d6671b1b440b6ca7de) and [`1dd4219`](https://github.com/sorbet-ai/clap-wrapper/commit/1dd42194a7a1581702d5b5b374b2415599de6f3a). `tests/wrapper-regressions` checks them (`ctest -R wrapper-regressions`): it passes on macOS and on Windows. With upstream's code it fails at the configure (the AU's name), and with upstream's sources alone, at the preset name. The Cocoa view check passes either way, because through `AudioUnitGetProperty` the AU SDK asks `GetPropertyInfo` first.

- **VST3 `ASSET_OUTPUT_DIRECTORY`:** the option was ignored on macOS and for a single-file Windows VST3. The code set the property on `VST3_TARGET`, which is unset, instead of `V3_TARGET`. The plugins now use it on every platform.
- **The AUv2's name with `CLAP_TARGET_FOR_CONFIG`:** `OUTPUT_NAME` was ignored, and the name was always the CLAP target's `LIBRARY_OUTPUT_NAME`. When that was unset, the configure step failed in `MAKE_C_IDENTIFIER`. Now `OUTPUT_NAME` is used when given, else the CLAP target's output name.
- **The AUv2's Info.plist after a re-configure:** with Ninja and Makefiles, CMake writes the bundle's generic plist again at every configure, and the AU's own plist was copied in only when the target linked. A re-configure therefore left a component that wasn't an AU. Now `<target>-info-plist` copies it in whenever either plist changes. It follows the target without depending on the binary's file, so signing the bundle afterwards doesn't make it run again. The plugins sign each AU after that target.
- **No Cocoa view without a gui:** `GetProperty(kAudioUnitProperty_CocoaUI)` returned a view for a plugin that has no gui extension, although `GetPropertyInfo` already refused one.
- **The AU preset name:** the class data carried the wrapper's own "Program" instead of the preset a host had set, which caused auval's "Preset name is not retained in retrieved class data" warning on every AU. It now carries the AU SDK's current preset, and restoring class data sets that preset again.

## clap-validator

The fork's `cargo test` passes, and clap-validator runs every one of its tests on the plugins, with no exclusions.

- [`941f60c`](https://github.com/sorbet-ai/clap-validator/commit/941f60c) **Preset discovery deadlock:** `begin_preset()` held its result lock across `flush_preset()`, which locks it again. As a result, `preset-discovery-crawl` and `preset-discovery-load` hung on any plugin with more than one preset in a location.
- [`36b337d`](https://github.com/sorbet-ai/clap-validator/commit/36b337d) **A refused sample rate:** `clap_plugin::activate()` may return false, and the plugins refuse rates outside the engines' 11.025 to 192 kHz. `process-varying-sample-rates` failed at the first rate a plugin refused, and so did the fuzzer. Both now skip refused rates, and the test fails only if every rate is refused.
- [`17f4f83`](https://github.com/sorbet-ai/clap-validator/commit/17f4f83) **Stepped parameters in param-set:** `param-set-events` and `param-set-no-cookies` check that the values change, but they could draw the current value: a stepped on/off parameter, half the time. They now send values that differ from the current ones.
