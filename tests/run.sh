#!/bin/sh
# Simple tests for livelog. Run with: make test
set -e

cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAIL=0

check() {   # name, expected file, actual file
    if diff -u "$2" "$3" >/dev/null; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        diff -u "$2" "$3" || true
        FAIL=1
    fi
}

cat > "$T/test.conf" <<EOF
command = cat
event_prefix = EVENT
match = SIP/100
match = SIP/200
match_mode = any
key = test-key-123
log_dir = $T/logs
file_prefix = ast-
keep_files = 20
restart_delay = 1
use_pty = no
strip_ansi = yes
EOF

# --- 1. chunk logic ---
printf '%s\n' \
    'Asterisk starting...' \
    'EVENT new call SIP/100' \
    '  dialing...' \
    'EVENT new call SIP/300' \
    '  dialing SIP/100' \
    'event lowercase SIP/100' \
    '  EVENT with space SIP/100' \
    'EVENT hangup SIP/200' \
    '  cause=16' \
    'event lowercase SIP/100' \
    'EVENT again SIP/100' \
    "$(printf '\033[1;32m  colored\033[0m\r')" > "$T/in.txt"

./livelog -c "$T/test.conf" -s < "$T/in.txt" 2>/dev/null
F=$(ls "$T/logs"/ast-*.log)
./livelog-decrypt "$F" test-key-123 | sed -E 's/^[0-9]{14}$/<TS>/' > "$T/out.txt"
printf '%s\n' \
    '<TS>' 'EVENT new call SIP/100' '  dialing...' \
    '<TS>' 'EVENT hangup SIP/200' '  cause=16' 'event lowercase SIP/100' \
    '<TS>' 'EVENT again SIP/100' '  colored' > "$T/exp.txt"
check "chunk logic" "$T/exp.txt" "$T/out.txt"

# --- 2. file is not plain text ---
if grep -q "SIP/100" "$F"; then echo "FAIL: file has plain text"; FAIL=1; else echo "PASS: file is encrypted"; fi

# --- 3. wrong key ---
if ./livelog-decrypt "$F" bad-key >/dev/null 2>&1; then echo "FAIL: wrong key accepted"; FAIL=1; else echo "PASS: wrong key detected"; fi

# --- 4. match_mode = all ---
rm -rf "$T/logs"
sed 's/match_mode = any/match_mode = all/' "$T/test.conf" > "$T/all.conf"
printf '%s\n' 'EVENT only SIP/100' 'a' 'EVENT SIP/100 and SIP/200' 'b' > "$T/in2.txt"
./livelog -c "$T/all.conf" -s < "$T/in2.txt" 2>/dev/null
./livelog-decrypt "$T/logs"/ast-*.log test-key-123 | sed -E 's/^[0-9]{14}$/<TS>/' > "$T/out2.txt"
printf '%s\n' '<TS>' 'EVENT SIP/100 and SIP/200' 'b' > "$T/exp2.txt"
check "match_mode all" "$T/exp2.txt" "$T/out2.txt"

# --- 5. keep only 20 files ---
rm -rf "$T/logs"; mkdir -p "$T/logs"
for d in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
    : > "$T/logs/ast-200001$d.log"
done
: > "$T/logs/other.txt"
printf 'EVENT SIP/100\n' | ./livelog -c "$T/test.conf" -s 2>/dev/null
N=$(ls "$T/logs"/ast-*.log | wc -l)
if [ "$N" -eq 20 ] && [ ! -e "$T/logs/ast-20000101.log" ] && [ -e "$T/logs/other.txt" ]; then
    echo "PASS: cleanup keeps 20 files"
else
    echo "FAIL: cleanup ($N files)"; FAIL=1
fi

# --- 6. command restarts after it stops ---
rm -rf "$T/logs"
sed 's/^command = cat$/command = echo "EVENT SIP\/100 run"/' "$T/test.conf" > "$T/restart.conf"
./livelog -c "$T/restart.conf" 2>/dev/null &
PID=$!
sleep 3.5
kill -TERM $PID
wait $PID || true
N=$(./livelog-decrypt "$T/logs"/ast-*.log test-key-123 | grep -c 'EVENT SIP/100 run')
if [ "$N" -ge 2 ]; then echo "PASS: command restarted ($N runs)"; else echo "FAIL: restart ($N runs)"; FAIL=1; fi

# --- 7. pty mode ---
rm -rf "$T/logs"
sed -e 's/use_pty = no/use_pty = yes/' -e 's/^command = cat$/command = printf "EVENT SIP\/100 pty\\nline2\\n"; sleep 5/' "$T/test.conf" > "$T/pty.conf"
./livelog -c "$T/pty.conf" 2>/dev/null &
PID=$!
sleep 1.5
kill -TERM $PID
wait $PID || true
./livelog-decrypt "$T/logs"/ast-*.log test-key-123 | sed -E 's/^[0-9]{14}$/<TS>/' > "$T/out7.txt"
printf '%s\n' '<TS>' 'EVENT SIP/100 pty' 'line2' > "$T/exp7.txt"
check "pty mode + fast stop" "$T/exp7.txt" "$T/out7.txt"

[ "$FAIL" -eq 0 ] && echo "ALL TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
