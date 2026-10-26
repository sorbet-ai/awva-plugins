#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The release archive of this platform's build: dist/awva-plugins-<version>-<platform>.zip, with
#   CLAP/awva_<engine>.clap   VST3/awva_<engine>.vst3   AU/awva_<engine>.component (macOS)
#   LICENSE NOTICE README.md
# (macOS: ditto, which keeps the bundles' signatures as they are.) Prints the archive's path.
# Usage: tools/package.sh PLATFORM      (e.g. windows-x64, macos-universal)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
B="$HERE/build"
platform="${1:?usage: tools/package.sh PLATFORM}"
version="$(sed -n 's/^project(awva_plugins VERSION \([0-9.]*\).*/\1/p' "$HERE/CMakeLists.txt")"
name="awva-plugins-$version-$platform"
out="$HERE/dist/$name"
rm -rf "$out" "$out.zip"
mkdir -p "$out/CLAP" "$out/VST3"
cp -R "$B"/awva_*.clap "$out/CLAP/"
cp -R "$B"/awva_*.vst3 "$out/VST3/"
if [ "$(uname -s)" = Darwin ]; then
  mkdir -p "$out/AU"
  cp -R "$B"/awva_*.component "$out/AU/"
fi
cp "$HERE/LICENSE" "$HERE/NOTICE" "$HERE/README.md" "$out/"
# every licence the binaries are under (NOTICE says which applies to what)
L="$out/licenses"
D="$HERE/deps"
A="awva-$(head -c 7 "$HERE/AWVA_COMMIT")"
mkdir -p "$L"
cp "$D/$A/LICENSE" "$L/awva-LICENSE.txt"
cp "$D/$A/NOTICE" "$L/awva-NOTICE.txt"
cp "$D/$A/PROVENANCE.md" "$L/awva-PROVENANCE.md" # (the fastapprox BSD-3-Clause notice)
cp "$D/clap-1.2.10/LICENSE" "$L/clap-LICENSE.txt"
cp "$D/clap-wrapper-0.17.0/LICENSE" "$L/clap-wrapper-LICENSE.txt"
sed -n '1,/\*\//p' "$D/clap-wrapper-0.17.0/libs/fmt/fmt/format.h" > "$L/fmt-LICENSE.txt"
cp "$D/vst3sdk-3.8.0/LICENSE.txt" "$L/vst3sdk-LICENSE.txt"
cp "$D/clap-wrapper-0.17.0/libs/psl/README.md" "$L/presonus-plugin-extensions-README.md" # (public domain)
if [ "$(uname -s)" = Darwin ]; then cp "$D/AudioUnitSDK-1.1.0/LICENSE.txt" "$L/AudioUnitSDK-LICENSE.txt"; fi
if [ "$(uname -s)" = Darwin ]; then
  (cd "$HERE/dist" && ditto -c -k --keepParent "$name" "$name.zip")
else
  (cd "$HERE/dist" && python -I -m zipfile -c "$name.zip" "$name")
fi
echo "dist/$name.zip"
