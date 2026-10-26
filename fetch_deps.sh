#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The dependencies, each into its own fresh directory under deps/. Each is checked against the commit
# or SHA-256 pinned below. Downloads are data: nothing here runs them; the build compiles the SDKs, and
# tools/validate.sh runs the validators.
#
#   deps/awva-<commit>         sorbet-ai/awva at AWVA_COMMIT: dsp/ sdk/ component/ presets/ tools/harness/
#                              third_party/doctest (from GitHub, or a local checkout: AWVA=<path>)  Apache-2.0
#   deps/clap-1.2.10           free-audio/clap 1.2.10                                       MIT
#   deps/vst3sdk-3.8.0         steinbergmedia/vst3sdk v3.8.0_build_66 (clap-wrapper 0.17.0's;
#                              base, pluginterfaces, public.sdk)                            MIT
#   deps/AudioUnitSDK-1.1.0    apple/AudioUnitSDK 1.1.0 (macOS)                             Apache-2.0
#   with --validators (tests only, never linked or shipped):
#   deps/clap-validator        built (cargo build --release --locked) from the submodule
#                              third_party/clap-validator, Sorbet AI's fork            MIT
#   deps/pluginval-1.0.4-<os>        Tracktion/pluginval v1.0.4 release                   GPLv3
# clap-wrapper and clap-validator are git submodules (third_party/: Sorbet AI's forks, pinned by
# commit; docs/forked_dependencies.md), checked out here first.
# Usage: ./fetch_deps.sh [--validators]      (AWVA=/path/to/awva: that checkout's commit, not GitHub's)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
D="$HERE/deps"
mkdir -p "$D"
case "$(uname -s)" in
Darwin) os=macos ;;
MINGW* | MSYS* | CYGWIN*) os=windows ;;
*) os=linux ;;
esac

git -C "$HERE" -c core.autocrlf=false submodule update --init third_party/clap-wrapper third_party/clap-validator

clone() { # dir url tag commit [submodule...]: the submodules at the commits the pinned commit records
  if [ -d "$D/$1" ]; then
    echo "have $1"
    return
  fi
  local dir="$1" url="$2" tag="$3" want="$4"
  shift 4
  rm -rf "$D/$dir.tmp"
  git -c advice.detachedHead=false -c core.longpaths=true -c core.autocrlf=false clone -q --depth 1 --branch "$tag" "$url" "$D/$dir.tmp"
  got="$(git -C "$D/$dir.tmp" rev-parse HEAD)"
  if [ "$got" != "$want" ]; then
    echo "fetch_deps.sh: $url $tag is $got, not the pinned $want" >&2
    exit 1
  fi
  if [ $# -gt 0 ]; then git -C "$D/$dir.tmp" -c core.longpaths=true -c core.autocrlf=false submodule update -q --init --depth 1 "$@"; fi
  mv "$D/$dir.tmp" "$D/$dir"
}
sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
download() { # dir url sha256: the archive, checked, unpacked in its own directory
  if [ -d "$D/$1" ]; then
    echo "have $1"
    return
  fi
  mkdir -p "$D/$1.tmp"
  curl -fsSL -o "$D/$1.tmp/download.zip" "$2"
  got="$(sha256 "$D/$1.tmp/download.zip")"
  if [ "$got" != "$3" ]; then
    echo "fetch_deps.sh: $2 has SHA-256 $got, not the pinned $3" >&2
    exit 1
  fi
  (cd "$D/$1.tmp" && unzip -q download.zip && for t in *.tar.gz; do [ -e "$t" ] && tar -xzf "$t"; done; true)
  mv "$D/$1.tmp" "$D/$1"
}

A="$(head -n 1 "$HERE/AWVA_COMMIT" | tr -d '\r')"
if [ ! -d "$D/awva-${A:0:7}" ]; then
  rm -rf "$D/awva-${A:0:7}.tmp" "$D/awva-src.tmp"
  mkdir -p "$D/awva-${A:0:7}.tmp"
  src="${AWVA:-}"
  if [ -z "$src" ]; then # the commit alone, from GitHub
    src="$D/awva-src.tmp"
    git init -q "$src"
    git -C "$src" fetch -q --depth 1 https://github.com/sorbet-ai/awva.git "$A"
  fi
  git -C "$src" archive "$A" dsp sdk component presets tools/harness third_party/doctest LICENSE NOTICE PROVENANCE.md | tar -x -C "$D/awva-${A:0:7}.tmp"
  rm -rf "$D/awva-src.tmp"
  mv "$D/awva-${A:0:7}.tmp" "$D/awva-${A:0:7}"
fi
clone clap-1.2.10 https://github.com/free-audio/clap.git 1.2.10 195b42a004144fab0b3cf95e9c067187d15365b7
# (the VST3 SDK: only what clap-wrapper compiles, not its docs, samples or VSTGUI)
clone vst3sdk-3.8.0 https://github.com/steinbergmedia/vst3sdk.git v3.8.0_build_66 9fad9770f2ae8542ab1a548a68c1ad1ac690abe0 base pluginterfaces public.sdk
if [ $os = macos ]; then
  clone AudioUnitSDK-1.1.0 https://github.com/apple/AudioUnitSDK.git AudioUnitSDK-1.1.0 53a9a2008aae7fb1b0a9f093dd523b9b12f6c0d9
fi

if [ "${1:-}" = --validators ]; then
  # clap-validator from the fork's source, its dependencies as its Cargo.lock pins them (Rust 1.95 or
  # later: its MSRV)
  cargo build -q --release --locked --manifest-path "$HERE/third_party/clap-validator/Cargo.toml" --bin clap-validator \
    --target-dir "$D/clap-validator"
  P=https://github.com/Tracktion/pluginval/releases/download/v1.0.4
  case $os in
  windows)
    download pluginval-1.0.4-windows "$P/pluginval_Windows.zip" c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab
    ;;
  macos)
    download pluginval-1.0.4-macos "$P/pluginval_macOS.zip" 3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f
    ;;
  linux)
    download pluginval-1.0.4-linux "$P/pluginval_Linux.zip" c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352
    ;;
  esac
fi
