#!/bin/sh
# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# auval -strict on one engine's AU, from an SSH session: auval only sees third-party components
# from the user's GUI session, so it runs as a one-shot launchd job in the user's own GUI domain
# (nothing shown on screen). The component is copied to ~/Library/Audio/Plug-Ins/Components for
# the run only and removed again afterwards, whatever the result.
# With AUVAL_DIRECT=1 (a CI runner, whose job runs in the logged-in session) auval runs directly.
# Usage (on the Mac, in the repo): tests/auval_mac.sh ENGINE    -> build/validate/ENGINE_auval.txt
set -u
E="${1:?usage: tests/auval_mac.sh ENGINE}"
B="$(cd "$(dirname "$0")/.." && pwd)/build"
V="$B/validate"
mkdir -p "$V"
C="$HOME/Library/Audio/Plug-Ins/Components"
AU="awva_$E.component"
CODES="$(grep "^$E " "$B/au_codes.txt" | cut -d' ' -f2-)" # "aumu Acco Awva"
OUT="$V/${E}_auval.txt"
LABEL="com.sorbet-ai.awva.auval"
if [ -z "$CODES" ]; then
  echo "auval_mac.sh: no AU codes for $E in $B/au_codes.txt" >&2
  exit 2
fi
if [ -e "$C/$AU" ]; then
  echo "auval_mac.sh: $C/$AU exists already: not touching it" >&2
  exit 2
fi
cat > "$V/auval_job.sh" <<EOS
#!/bin/sh
auval -strict -v $CODES > "$OUT" 2>&1
echo "rc=\$?" >> "$OUT"
EOS
chmod +x "$V/auval_job.sh"
cat > "$V/auval_job.plist" <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$LABEL</string>
<key>ProgramArguments</key><array><string>$V/auval_job.sh</string></array>
<key>RunAtLoad</key><true/>
</dict></plist>
EOP
cleanup() {
  [ "${AUVAL_DIRECT:-0}" = 1 ] || launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null
  rm -rf "$C/$AU"
  killall -9 AudioComponentRegistrar 2>/dev/null
}
trap cleanup EXIT
mkdir -p "$C"
cp -R "$B/$AU" "$C/"
killall -9 AudioComponentRegistrar 2>/dev/null
sleep 5
rm -f "$OUT"
if [ "${AUVAL_DIRECT:-0}" = 1 ]; then
  "$V/auval_job.sh"
else
  launchctl bootstrap "gui/$(id -u)" "$V/auval_job.plist"
fi
i=0
while [ $i -lt 100 ] && ! grep -q "^rc=" "$OUT" 2>/dev/null; do
  sleep 3
  i=$((i + 1))
done
grep -E "AU VALIDATION SUCCEEDED|FAIL|FATAL|ERROR|WARNING|^rc=" "$OUT"
