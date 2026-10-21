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

# --- 8. FTP upload (needs python3 for the fake FTP server) ---
if command -v python3 >/dev/null 2>&1; then
    rm -rf "$T/logs" "$T/ftp"; mkdir -p "$T/ftp"
    ./livelog -c "$T/test.conf" -s < "$T/in.txt" 2>/dev/null
    TODAY=$(ls "$T/logs"/ast-*.log)
    for d in 20000101 20000102 20000103; do cp "$TODAY" "$T/logs/ast-$d.log"; done
    echo extra >> "$T/logs/ast-20000102.log"
    cat "$T/test.conf" > "$T/ftp.conf"
    printf '%s\n' 'ftp_server = 127.0.0.1' 'ftp_user = u' 'ftp_password = p' \
        'ftp_path = /a/b' 'ftp_retries = 3' 'ftp_retry_delay = 1' 'ftp_timeout = 5' >> "$T/ftp.conf"

    start_ftpd() {   # extra options for the server
        rm -f "$T/port"
        python3 tests/fake_ftpd.py "$T/ftp" "$T/port" "$@" &
        FTPD=$!
        for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/port" ] && break; sleep 0.5; done
        sed -i '/^ftp_port/d' "$T/ftp.conf"
        echo "ftp_port = $(cat "$T/port")" >> "$T/ftp.conf"
    }
    same_files() {   # every local log file is on the server with the same content
        for f in "$T"/logs/ast-*.log; do
            cmp -s "$f" "$T/ftp/a/b/$(basename "$f")" || return 1
        done
    }

    # 8a. all files are uploaded, the folder is created
    start_ftpd
    ./livelog -c "$T/ftp.conf" -u 2>/dev/null
    if same_files; then echo "PASS: ftp upload of all files"; else echo "FAIL: ftp upload of all files"; FAIL=1; fi

    # 8b. a half written last record is not uploaded; a file with a wrong size is uploaded again
    cp "$TODAY" "$T/today.copy"
    printf 'half-record' >> "$TODAY"
    : > "$T/ftp/a/b/ast-20000101.log"
    ./livelog -c "$T/ftp.conf" -u 2>/dev/null
    if cmp -s "$T/today.copy" "$T/ftp/a/b/$(basename "$TODAY")" &&
       cmp -s "$T/logs/ast-20000101.log" "$T/ftp/a/b/ast-20000101.log"; then
        echo "PASS: ftp snapshot cut + size check"
    else
        echo "FAIL: ftp snapshot cut + size check"; FAIL=1
    fi
    kill $FTPD; wait $FTPD 2>/dev/null || true

    # 8c. server refuses the first 2 uploads and has no EPSV: retries and PASV fallback
    rm -rf "$T/ftp"; mkdir -p "$T/ftp"
    truncate -s -11 "$TODAY"
    start_ftpd --fail-stor 2 --no-epsv
    ./livelog -c "$T/ftp.conf" -u 2>/dev/null
    if same_files; then echo "PASS: ftp retry + PASV"; else echo "FAIL: ftp retry + PASV"; FAIL=1; fi
    kill $FTPD; wait $FTPD 2>/dev/null || true

    # 8d. no server: the round ends (retries for 2 newest files only), no crash
    sed -i '/^ftp_port/d' "$T/ftp.conf"; echo 'ftp_port = 1' >> "$T/ftp.conf"
    if ./livelog -c "$T/ftp.conf" -u 2>"$T/err8.txt"; then
        N=$(grep -c 'cannot connect' "$T/err8.txt")
        # 2 newest files: 1 try + 3 retries each = 8 tries; the 3rd file stops the round
        if [ "$N" -eq 9 ]; then echo "PASS: ftp server down"; else echo "FAIL: ftp server down ($N tries)"; FAIL=1; fi
    else
        echo "FAIL: ftp server down (exit code)"; FAIL=1
    fi
else
    echo "SKIP: ftp tests (python3 not found)"
fi

# --- 9. livelog-decrypt without arguments: key from livelog.conf, all *.log in the folder ---
rm -rf "$T/logs" "$T/d"; mkdir -p "$T/d"
./livelog -c "$T/test.conf" -s < "$T/in.txt" 2>/dev/null
cp "$T"/logs/ast-*.log "$T/d/ast-20000101.log"
cp "$T"/logs/ast-*.log "$T/d/ast-20000102.log"
cp "$T"/logs/ast-*.log "$T/orig.log"
echo "just text" > "$T/d/plain.log"
cp livelog-decrypt "$T/d/"
cp "$T/test.conf" "$T/d/livelog.conf"
"$T/d/livelog-decrypt" > "$T/dec1.txt" 2>&1
if cmp -s "$T/orig.log" "$T/d/ast-20000101.log.bak" && cmp -s "$T/orig.log" "$T/d/ast-20000102.log.bak" &&
   [ "$(cat "$T/d/plain.log")" = "just text" ] && [ ! -e "$T/d/plain.log.bak" ] &&
   sed -E 's/^[0-9]{14}$/<TS>/' "$T/d/ast-20000101.log" | cmp -s - "$T/exp.txt"; then
    echo "PASS: decrypt without arguments"
else
    echo "FAIL: decrypt without arguments"; cat "$T/dec1.txt"; FAIL=1
fi
# second run: nothing to do, .bak files stay
"$T/d/livelog-decrypt" > "$T/dec2.txt" 2>&1
if grep -q '0 decrypted, 3 already decrypted, 0 failed' "$T/dec2.txt" &&
   cmp -s "$T/orig.log" "$T/d/ast-20000101.log.bak"; then
    echo "PASS: decrypted files are skipped"
else
    echo "FAIL: decrypted files are skipped"; cat "$T/dec2.txt"; FAIL=1
fi
# wrong key in livelog.conf: file stays as it is
cp "$T/orig.log" "$T/d/ast-20000103.log"
sed -i 's/^key = .*/key = wrong-key/' "$T/d/livelog.conf"
if ! "$T/d/livelog-decrypt" >/dev/null 2>&1 && cmp -s "$T/orig.log" "$T/d/ast-20000103.log" &&
   [ ! -e "$T/d/ast-20000103.log.bak" ] && [ ! -e "$T/d/ast-20000103.log.tmp" ]; then
    echo "PASS: wrong key in livelog.conf keeps the file"
else
    echo "FAIL: wrong key in livelog.conf keeps the file"; FAIL=1
fi

[ "$FAIL" -eq 0 ] && echo "ALL TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
