#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# Build and test the awva plugins natively.
#   Windows (Git Bash): clang with Visual Studio's libraries (found with vswhere)
#   macOS:              Xcode's clang; cmake and ninja from Homebrew (/opt/homebrew/bin)
# Usage: ./build.sh [build|test|all] [ctest arguments...]     (build dir: build/)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
what="${1:-all}"
[ $# -gt 0 ] && shift
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
  vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  vs="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
  bat="$(mktemp -d)/build.bat"
  in_env() {
    {
      echo "@call \"$vs\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul || exit /b 1"
      echo "@cd /d \"$(cygpath -w "$HERE")\" || exit /b 1"
      echo "@$*"
    } > "$bat"
    cmd //c "$(cygpath -w "$bat")"
  }
  gen="-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
  ;;
Darwin)
  export PATH="/opt/homebrew/bin:$PATH"
  in_env() { (cd "$HERE" && eval "$*"); }
  gen="-G Ninja"
  ;;
*)
  in_env() { (cd "$HERE" && eval "$*"); }
  gen="-G Ninja"
  ;;
esac
if [ "$what" = build ] || [ "$what" = all ]; then
  in_env "cmake -S . -B build $gen -DCMAKE_BUILD_TYPE=Release"
  in_env "cmake --build build"
fi
if [ "$what" = test ] || [ "$what" = all ]; then
  in_env "ctest --test-dir build -L plugin --output-on-failure $*"
fi
