#!/bin/sh
# Build and install livelog as a systemd service.
# Run as root from the project folder:  sudo sh install.sh
#
# Needed files in this folder: compile.sh, src/, and a config:
#   livelog.conf          (your config, used first)
#   livelog.conf.example  (used if livelog.conf is not here)
# /opt/livelog/livelog.conf is overwritten every time (the old one is saved as livelog.conf.bak).
set -e

PREFIX=/opt/livelog
UNIT=/etc/systemd/system/livelog.service
START_DELAY=30      # seconds to wait after boot before starting

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root (sudo sh install.sh)"
    exit 1
fi
cd "$(dirname "$0")"

# --- check files first, before changing anything ---
if [ -f livelog.conf ]; then
    CONF_SRC=livelog.conf
elif [ -f livelog.conf.example ]; then
    CONF_SRC=livelog.conf.example
else
    echo "No config found. Put livelog.conf (or livelog.conf.example) in $(pwd)"
    exit 1
fi
if [ ! -f compile.sh ]; then
    echo "Missing file: $(pwd)/compile.sh"
    exit 1
fi

# --- build ---
sh compile.sh

# --- install programs and config ---
install -d -m 755 "$PREFIX"
install -m 755 livelog livelog-decrypt "$PREFIX/"
if [ -f "$PREFIX/livelog.conf" ]; then
    install -m 600 "$PREFIX/livelog.conf" "$PREFIX/livelog.conf.bak"
    echo "Old config saved as $PREFIX/livelog.conf.bak"
fi
install -m 600 "$CONF_SRC" "$PREFIX/livelog.conf"
echo "Config installed from $CONF_SRC to $PREFIX/livelog.conf"

# --- systemd service ---
cat > "$UNIT" <<EOF
[Unit]
Description=livelog - live log processor (encrypted chunk logger)
After=network-online.target asterisk.service
Wants=network-online.target

[Service]
Type=simple
# Wait after boot so asterisk is ready.
ExecStartPre=/bin/sleep $START_DELAY
ExecStart=$PREFIX/livelog
Restart=always
RestartSec=10
TimeoutStartSec=$((START_DELAY + 90))
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
EOF
chmod 644 "$UNIT"
systemctl daemon-reload
systemctl enable livelog.service

# --- start, only if the key is set ---
echo
if grep -Eq '^[[:space:]]*key[[:space:]]*=[[:space:]]*"?change-me-please"?[[:space:]]*$' "$PREFIX/livelog.conf" ||
   ! grep -Eq '^[[:space:]]*key[[:space:]]*=' "$PREFIX/livelog.conf"; then
    echo "Installed, but NOT started: set your own 'key' in the config first:"
    echo "    nano $PREFIX/livelog.conf"
    echo "Then start the service:"
    echo "    systemctl start livelog"
else
    systemctl restart --no-block livelog.service
    echo "Installed and (re)started. It waits $START_DELAY s before it runs the command."
    echo "Check with:  systemctl status livelog"
    echo "Messages:    journalctl -u livelog -f"
fi
