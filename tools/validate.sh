#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The validators on every built plugin, on this machine (Windows in Git Bash, or macOS), from
# ./fetch_deps.sh --validators:
#   clap-validator 0.4.1 on the .clap, every test but these known artefacts of the validator:
#     preset-discovery-crawl, -load   deadlock inside clap-validator 0.4.1 itself (begin_preset holds
#                                     its result mutex and flush_preset() locks it again)
#     process-varying-sample-rates    activates at 1 kHz to 768 kHz and fails on the first refusal: the
#                                     plugins run the engines' rates only (nsc_desc min_rate .. max_rate,
#                                     11.025 to 192 kHz; activate() refuses others, as CLAP allows); the
#                                     info proof checks that range
#   pluginval 1.0.4      --strictness-level 10 --skip-gui-tests on the .vst3
#   auval -strict        on the .component (macOS: tests/auval_mac.sh, the component installed for
#                        the run only)
# Logs in build/validate/, a table on stdout (and in build/validate/table.md); exit 1 if any failed.
# Nothing opens an audio device.
# Usage: tools/validate.sh [engine...]      (default: every plugin in build/)
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
B="$HERE/build"
V="$B/validate"
mkdir -p "$V"
case "$(uname -s)" in
Darwin)
  CV="$(find "$HERE"/deps/clap-validator-0.4.1-macos -type f -name clap-validator -perm -u+x | head -n 1)"
  PV="$(find "$HERE"/deps/pluginval-1.0.4-macos -type f -path '*/Contents/MacOS/pluginval' | head -n 1)"
  mac=1
  ;;
*)
  CV="$(find "$HERE"/deps/clap-validator-0.4.1-windows -type f -name clap-validator.exe | head -n 1)"
  PV="$(find "$HERE"/deps/pluginval-1.0.4-windows -type f -name pluginval.exe | head -n 1)"
  mac=0
  ;;
esac
if [ ! -x "$CV" ] || [ ! -x "$PV" ]; then
  echo "validate.sh: no validators in deps/ (./fetch_deps.sh --validators)" >&2
  exit 2
fi
engines=("$@")
if [ ${#engines[@]} -eq 0 ]; then
  for c in "$B"/awva_*.clap; do
    n="$(basename "$c" .clap)"
    engines+=("${n#awva_}")
  done
fi

one() { # engine: the CLAP and VST3 validators, in parallel with the other engines
  e="$1"
  x='preset-discovery-(crawl|load)|process-varying-sample-rates'
  "$CV" validate -x "$x" "$B/awva_$e.clap" > "$V/${e}_clap-validator.log" 2>&1
  echo "rc=$?" >> "$V/${e}_clap-validator.log"
  "$PV" --strictness-level 10 --skip-gui-tests --timeout-ms 300000 --validate "$B/awva_$e.vst3" > "$V/${e}_pluginval.log" 2>&1
  echo "rc=$?" >> "$V/${e}_pluginval.log"
}
pids=()
for e in "${engines[@]}"; do
  one "$e" &
  pids+=($!)
  if [ ${#pids[@]} -ge 5 ]; then
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  fi
done
wait
if [ $mac = 1 ]; then # one at a time: auval and the registrar are per user
  for e in "${engines[@]}"; do
    "$HERE/tests/auval_mac.sh" "$e" > /dev/null 2>&1
  done
fi

cv_result() { # the summary line, e.g. "40 tests run, 34 passed, 0 failed, 0 warnings, 6 skipped"
  l="$V/${1}_clap-validator.log"
  s="$(sed 's/\x1b\[[0-9;]*m//g' "$l" 2> /dev/null | grep -E '[0-9]+ tests run' | tail -1 | sed 's/^ *//')"
  if grep -q '^rc=0' "$l" 2> /dev/null; then echo "pass ($s)"; else echo "FAIL ($s)"; fi
}
pv_result() {
  l="$V/${1}_pluginval.log"
  if grep -q '^rc=0' "$l" 2> /dev/null && grep -q 'SUCCESS' "$l"; then echo "pass"; else echo "FAIL"; fi
}
au_result() {
  l="$V/${1}_auval.txt"
  if grep -q '^rc=0' "$l" 2> /dev/null && grep -q 'AU VALIDATION SUCCEEDED' "$l"; then
    echo "pass ($(grep -c 'WARNING' "$l") warnings)"
  else echo "FAIL"; fi
}
{
  printf '| engine | clap-validator | pluginval 10 (VST3) |%s\n' "$([ $mac = 1 ] && echo ' auval -strict (AU) |')"
  printf '|---|---|---|%s\n' "$([ $mac = 1 ] && echo '---|')"
  for e in "${engines[@]}"; do
    c="$(cv_result "$e")"
    p="$(pv_result "$e")"
    a=""
    [ $mac = 1 ] && a=" $(au_result "$e") |"
    printf '| %s | %s | %s |%s\n' "$e" "$c" "$p" "$a"
  done
} > "$V/table.md"
cat "$V/table.md"
if grep -q 'FAIL' "$V/table.md"; then exit 1; fi
exit 0
