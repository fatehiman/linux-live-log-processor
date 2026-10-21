#!/bin/sh
# Build and install livelog as a systemd service.
# Run as root from the project folder:  sudo sh install.sh
set -e

PREFIX=/opt/livelog

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root (sudo sh install.sh)"
    exit 1
fi
cd "$(dirname "$0")"
sh compile.sh

install -d -m 755 "$PREFIX"
install -m 755 livelog livelog-decrypt "$PREFIX/"

NEW_CONFIG=0
if [ ! -f "$PREFIX/livelog.conf" ]; then
    install -m 600 livelog.conf.example "$PREFIX/livelog.conf"
    NEW_CONFIG=1
fi

install -m 644 systemd/livelog.service /etc/systemd/system/livelog.service
systemctl daemon-reload
systemctl enable livelog.service

if [ "$NEW_CONFIG" -eq 1 ]; then
    echo
    echo "Installed. Now edit the config (set 'key', 'command', 'match'):"
    echo "    nano $PREFIX/livelog.conf"
    echo "Then start the service:"
    echo "    systemctl start livelog"
else
    systemctl restart livelog.service
    echo "Updated and restarted. Check with: systemctl status livelog"
fi
