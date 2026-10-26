#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# One iteration of an engine for the awva app: an engine change (in an awva checkout) as a package
# (<engine>-iter<n>.awvapkg, awva-package/2) the running app lists within two seconds, next to the
# built-in engine it changes.
#
#   app/tools/iterate.sh ENGINE AWVA_DIR [options]
#     ENGINE           accordion bowed brass ep modal reed sine trumpet va wind
#     AWVA_DIR         the awva checkout with the change, on the app's awva commit (AWVA_COMMIT):
#                      its nsc.h must be that commit's (the package format's pin)
#   options
#     --presets-only   the change is in presets/<ENGINE>/ only: a presets-only package, no build
#     --label TEXT     the name the app lists ("<engine> iter <n>" by default)
#     --engines DIR    the packages folder (default: the app's: %APPDATA%\awva\engines,
#                      ~/Library/Application Support/awva/engines)
#     --cli PATH       awva_app_cli (default: build/app/awva_app_cli of this repository), for the check
#     --packer PATH    the packager (default: AWVA_DIR/component/tools/nsc_pack.py)
#
# What it does: builds the engine's component library from AWVA_DIR (awva's sdk, in AWVA_DIR/build/
# iterate, incremental: the first build compiles every engine once), with its presets compiled in;
# packs it with awva's packager (this computer's image, the manifest, the label, the awva commit) as
# <ENGINE>-iter<n>.awvapkg (the next free n), written as .tmp and renamed, so the app never sees half
# of one; then checks it as the app would play it (every preset rendered offline: finite, sounding)
# and compares it with the built-in engine, preset by preset.
# Nothing opens an audio device.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
t0=$(date +%s)
die() {
  echo "iterate: $*" >&2
  exit 1
}

[ $# -ge 2 ] || die "usage: app/tools/iterate.sh ENGINE AWVA_DIR [--presets-only] [--label TEXT] [--engines DIR] [--cli PATH]"
engine="$1"
awva="$(cd "$2" && pwd)"
shift 2
presets_only=0 label="" engines="" cli="" packer=""
while [ $# -gt 0 ]; do
  case "$1" in
  --presets-only) presets_only=1 ;;
  --label) label="$2"; shift ;;
  --engines) engines="$2"; shift ;;
  --cli) cli="$2"; shift ;;
  --packer) packer="$2"; shift ;;
  *) die "unknown option $1" ;;
  esac
  shift
done
case "$engine" in
accordion | bowed | brass | ep | modal | reed | sine | trumpet | va | wind) ;;
*) die "$engine isn't an awva engine" ;;
esac

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) os=windows suffix=.dll exe=.exe ;;
Darwin) os=macos suffix=.dylib exe= ;;
*) os=linux suffix=.so exe= ;;
esac
native() { if [ "$os" = windows ]; then cygpath -w "$1"; else echo "$1"; fi; }
if [ -z "$engines" ]; then
  if [ "$os" = windows ]; then engines="$(cygpath -u "$APPDATA")/awva/engines"; else engines="$HOME/Library/Application Support/awva/engines"; fi
fi
mkdir -p "$engines"
[ -n "$cli" ] || cli="$HERE/build/app/awva_app_cli$exe"
[ -x "$cli" ] || die "no $cli: build the app's core first (./build.sh build)"
[ -n "$packer" ] || packer="$awva/component/tools/nsc_pack.py"
[ -f "$packer" ] || die "no packager $packer (awva's component/tools/nsc_pack.py)"
py="$(command -v python3 || command -v python)"
case "$os" in
windows) tag=x86_64-windows ;;
macos) if [ "$(uname -m)" = arm64 ]; then tag=arm64-macos; else tag=x86_64-macos; fi ;;
*) tag=x86_64-linux ;;
esac

# the base: the app's awva commit, and its nsc.h (the package format's pin)
base="$(head -n 1 "$HERE/AWVA_COMMIT" | tr -d '\r')"
short="${base:0:7}"
git -C "$awva" cat-file -e "$base^{commit}" 2> /dev/null || die "$awva doesn't have the app's awva commit $short"
git -C "$awva" merge-base --is-ancestor "$base" HEAD || die "$awva isn't on $short (the app plays packages built on it)"
git -C "$awva" diff --quiet "$base" -- component/include/nsc/nsc.h || die "$awva's nsc.h differs from $short's: not a package this app loads"

# the next iteration
n=1
for d in "$engines/$engine"-iter*.awvapkg; do
  [ -f "$d" ] || continue
  k="${d##*-iter}"
  k="${k%.awvapkg}"
  case "$k" in '' | *[!0-9]*) continue ;; esac
  [ "$k" -ge "$n" ] && n=$((k + 1))
done
name="$engine-iter$n"
[ -n "$label" ] || label="$engine iter $n"
out="$engines/$name.awvapkg"
tmp="$out.tmp"
work="$(mktemp -d)"
trap 'rm -rf "$tmp" "$work"' EXIT

if [ "$presets_only" = 1 ]; then
  ls "$awva/presets/$engine/"*.preset.json > /dev/null 2>&1 || die "$awva/presets/$engine has no presets"
  "$py" "$(native "$packer")" package "$(native "$tmp")" --engine "$engine" --label "$label" --presets "$(native "$awva/presets/$engine")" > /dev/null
else
  # the presets into the engine (presets/*.preset.json -> dsp/include/awva/presets/*.inc), then the
  # component library: awva's sdk build, with the toolchain the app's built-ins are built with
  run() {
    if [ "$os" = windows ]; then
      local vs bat
      vs="$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
      bat="$(mktemp -d)/run.bat"
      { echo "@call \"$vs\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul 2>&1 || exit /b 1"; echo "@cd /d \"$(cygpath -w "$awva")\" || exit /b 1"; echo "@$*"; } > "$bat"
      cmd //c "$(cygpath -w "$bat")"
    else
      (cd "$awva" && eval "$*")
    fi
  }
  gen="-G Ninja -DCMAKE_BUILD_TYPE=Release"
  [ "$os" = windows ] && gen="$gen -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  run "cmake -P tools/presets_inc.cmake" > /dev/null
  # the version the app shows: the checkout's commit, "-dirty" with uncommitted changes
  version="$(git -C "$awva" rev-parse --short=7 HEAD)"
  [ -z "$(git -C "$awva" status --porcelain -- dsp sdk component presets)" ] || version="$version-dirty"
  [ -f "$awva/build/iterate/CMakeCache.txt" ] || echo "iterate: configuring $awva/build/iterate (the first build compiles every engine once)"
  run "cmake -S sdk -B build/iterate $gen -DAWVA_VERSION=$version" > /dev/null
  echo "iterate: building $engine ($version)"
  run "cmake --build build/iterate --target nsc_$engine" | grep -E "error|FAILED" || true
  lib="$awva/build/iterate/components/$engine$suffix"
  man="$awva/build/iterate/components/$engine.json"
  [ -f "$lib" ] && [ -f "$man" ] || die "the build gave no $lib (and its manifest)"
  toolchain="$(clang --version 2> /dev/null | head -n 1 | grep -o 'clang version [0-9.]*' | sed 's/ version//')"
  "$py" "$(native "$packer")" host "$(native "$lib")" "$(native "$man")" "$tag" "${toolchain:-clang}" "$(native "$work/$engine.nscm")" > /dev/null
  "$py" "$(native "$packer")" package "$(native "$tmp")" --image "$tag=$(native "$work/$engine.nscm")" --manifest "$(native "$man")" \
    --label "$label" --awva "$short" > /dev/null
fi

# into place in one step (the app lists it within two seconds)
mv "$tmp" "$out"
trap 'rm -rf "$work"' EXIT
echo "iterate: wrote $(native "$out") \"$label\""

# as the app would play it, and against the built-in engine
"$cli" check --engine "package:$name" --against "builtin:$engine" --engines "$(native "$engines")" || {
  echo "iterate: the package doesn't pass its check; it stays listed with the reason (remove: rm \"$out\")" >&2
  exit 1
}
echo "iterate: $name ready in $(($(date +%s) - t0)) s: in the app, choose \"$label\" under \"your engines\""
