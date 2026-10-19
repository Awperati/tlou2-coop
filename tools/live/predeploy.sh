#!/bin/bash
# Pre-deploy checklist (2026-10-20). Read-only unless --deploy is given; never brings a game to the front.
#   tools/live/predeploy.sh            check only
#   tools/live/predeploy.sh --deploy   check, then deploy.cmd, then wait until both PCs run the new build
# Checks (5: the handshake is "each PC's mod log restarted after the deploy", by that PC's own clock):
#   1. build: coop_mod + coop_tests (the slow patch_race test is skipped), then coop_tests passes
#   2. git: nothing uncommitted under src/ or tests/ (a deploy should be a commit)
#   3. LAN file drop (tools/share.py, port 8765) answers, so PC B can pull the dll
#   4. PC B reachable over SSH; game and mod launcher running on both PCs (PIDs)
#   5. with --deploy: both PCs' tlou2-coop.log restarted after the deploy, within 120 s
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
G="/e/Games/The Last of Us - Part II Remastered"
B_LOG='E:\Games\The Last of Us - Part II Remastered\tlou2-coop.log'
ssh_b() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes -o ConnectTimeout=5 sharn@192.168.0.238 "$@" 2>/dev/null; }
fail=0; ok() { echo "  ok   $*"; }; bad() { echo "  FAIL $*"; fail=1; }; warn() { echo "  warn $*"; }

echo "1. build and tests"
VS='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
tmp="$(mktemp -d)"; cat > "$tmp/b.cmd" <<EOF
@echo off
call "$VS\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1
"$VS\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe" -S "$(cygpath -w "$ROOT")" -B "$(cygpath -w "$ROOT/build")" -G Ninja -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
"$VS\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe" --build "$(cygpath -w "$ROOT/build")" --target coop_mod coop_tests
EOF
if out="$(cmd //c "$(cygpath -w "$tmp/b.cmd")" 2>&1)"; then ok "coop_mod + coop_tests built"; else bad "build failed"; echo "$out" | grep -aE "error|FAILED" | head -5; fi
rm -rf "$tmp"
res="$("$ROOT/build/coop_tests.exe" 2>&1 | tail -1)"
case "$res" in *" 0 failed"*) ok "tests: $res";; *) bad "tests: $res";; esac
# The log's "build" stamp is dllmain.cpp's __DATE__/__TIME__ (only changes when that file compiles), so the
# handshake below checks that each PC's mod restarted after the deploy instead.
ok "coop.dll $(date -r "$ROOT/build/coop.dll" +%H:%M:%S), $(stat -c %s "$ROOT/build/coop.dll") bytes"

echo "2. git"
dirty="$(cd "$ROOT" && git status --short -- src tests | grep -v '^??')"
[ -z "$dirty" ] && ok "src/ and tests/ committed ($(cd "$ROOT" && git log --oneline -1 | cut -c1-60))" || { warn "uncommitted changes (another session may be mid-edit):"; echo "$dirty" | sed 's/^/         /'; }

echo "3. LAN file drop"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8765/coop.dll)"
[ "$code" = 200 ] && ok "share.py answers (HTTP 200)" || bad "share.py not answering (HTTP $code): start 'python -I tools/share.py' first"

echo "4. games"
a_game="$(tasklist //FI 'IMAGENAME eq tlou-ii.exe' //FO CSV //NH 2>/dev/null | grep -o '"[0-9]*"' | head -1 | tr -d '"')"
a_launch="$(tasklist //FI 'IMAGENAME eq coop_launcher.exe' //FO CSV //NH 2>/dev/null | grep -o '"[0-9]*"' | head -1 | tr -d '"')"
[ -n "$a_game" ] && ok "PC A game pid $a_game" || warn "PC A game not running (deploy only copies files)"
[ -n "$a_launch" ] && ok "PC A mod launcher pid $a_launch" || warn "PC A mod launcher not running"
if ssh_b "echo up" | grep -q up; then
  ok "PC B reachable"
  b_game="$(ssh_b 'tasklist /FI "IMAGENAME eq tlou-ii.exe" /FO CSV /NH' | grep -o '"[0-9]*"' | head -1 | tr -d '"')"
  [ -n "$b_game" ] && ok "PC B game pid $b_game" || warn "PC B game not running"
else bad "PC B not reachable over SSH"; fi

if [ "${1:-}" != "--deploy" ]; then
  echo; [ $fail = 0 ] && echo "checks passed (no deploy: pass --deploy)" || echo "checks FAILED"; exit $fail
fi
[ $fail = 0 ] || { echo "not deploying: fix the failures above"; exit 1; }

echo "5. deploy and reload handshake"
t_a="$(date +%H:%M:%S)"
t_b="$(ssh_b "powershell -NoProfile -c \"(Get-Date).ToString('HH:mm:ss')\"" | tr -d '\r')"
(cd "$ROOT" && cmd //c ".\deploy.cmd") | sed 's/^/  /'
started() { echo "$1" | grep -aoE '^\[[0-9:]{8}' | tr -d '['; }   # HH:MM:SS of a log's first line
first_a() { head -1 "$G/tlou2-coop.log" 2>/dev/null; }
first_b() { ssh_b "powershell -NoProfile -c \"Get-Content '$B_LOG' -TotalCount 1\"" | tr -d '\r'; }
ga=0; gb=0; la=""; lb=""
for i in $(seq 1 24); do
  la="$(first_a)"; lb="$(first_b)"
  sa="$(started "$la")"; sb="$(started "$lb")"
  [ -n "$a_game" ] && [ -n "$sa" ] && ! [[ "$sa" < "$t_a" ]] && ga=1
  [ -n "${b_game:-}" ] && [ -n "$sb" ] && ! [[ "$sb" < "$t_b" ]] && gb=1
  { [ -z "$a_game" ] || [ $ga = 1 ]; } && { [ -z "${b_game:-}" ] || [ $gb = 1 ]; } && break
  sleep 5
done
[ -z "$a_game" ] || { [ $ga = 1 ] && ok "PC A reloaded: ${la:0:80}" || bad "PC A did not reload: ${la:0:80}"; }
[ -z "${b_game:-}" ] || { [ $gb = 1 ] && ok "PC B reloaded: ${lb:0:80}" || bad "PC B did not reload: ${lb:0:80}"; }
echo; [ $fail = 0 ] && echo "deployed; both running PCs reloaded the new build" || echo "deploy check FAILED"; exit $fail
