#!/bin/sh
# Compile livelog and livelog-decrypt on this server.
# It only builds. It does not install anything.
#   sh compile.sh          build
#   sh compile.sh test     build and run the tests
# To install as a service after this, run: sudo sh install.sh
set -e

cd "$(dirname "$0")"

if ! command -v gcc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
    echo "gcc and make are needed."
    echo "Debian/Ubuntu: apt install build-essential"
    echo "CentOS/RHEL:   yum install gcc make"
    exit 1
fi

make clean
make

if [ "$1" = "test" ]; then
    make test
fi

echo
echo "Done. Programs built in $(pwd):"
ls -l livelog livelog-decrypt
