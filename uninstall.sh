#!/bin/sh
# Remove the livelog service and programs.
# The config file and the log files are kept.
set -e

PREFIX=/opt/livelog

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root (sudo sh uninstall.sh)"
    exit 1
fi

systemctl stop livelog.service 2>/dev/null || true
systemctl disable livelog.service 2>/dev/null || true
rm -f /etc/systemd/system/livelog.service
systemctl daemon-reload
rm -f "$PREFIX/livelog" "$PREFIX/livelog-decrypt"

echo "Removed. Kept: $PREFIX/livelog.conf and the log files (see log_dir in the config)."
