#!/bin/sh
# Compile livelog and livelog-decrypt on this server.
# It only builds. It does not install anything.
# It needs only gcc (no make, no Makefile).
#   sh compile.sh          build
#   sh compile.sh test     build and run the tests
# To install as a service after this, run: sudo sh install.sh
set -e

cd "$(dirname "$0")"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -Wall -Wextra"}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "gcc is needed."
    echo "Debian/Ubuntu: apt install gcc"
    echo "CentOS/RHEL:   yum install gcc"
    exit 1
fi
for f in src/livelog.c src/livelog-decrypt.c src/rc4.h; do
    if [ ! -f "$f" ]; then
        echo "Missing file: $(pwd)/$f"
        echo "Copy the whole project folder (with the src folder), or use git clone."
        exit 1
    fi
done

rm -f livelog livelog-decrypt
echo "Building livelog ..."
$CC $CFLAGS -o livelog src/livelog.c -lutil
echo "Building livelog-decrypt ..."
$CC $CFLAGS -o livelog-decrypt src/livelog-decrypt.c

if [ "$1" = "test" ]; then
    sh tests/run.sh
fi

echo
echo "Done. Programs built in $(pwd):"
ls -l livelog livelog-decrypt
