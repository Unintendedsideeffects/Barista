#!/bin/sh
set -eu
cd /opt/barista/examples/on-demand
test -f .env || { echo "Create .env from .env.example first" >&2; exit 1; }
chmod 0600 .env
install -d -m 0755 run
install -d -m 0700 state state/radio
install -d -o 1000 -g 1000 -m 0700 state/browser
chown -R 1000:1000 state/browser
test -f manager.json || install -m 0644 runtime/manager.json.example manager.json
install -m 0644 runtime/barista-gamepad-manager.service /etc/systemd/system/
ln -sf /opt/barista/examples/on-demand/runtime/manager.py /usr/local/bin/gamepadctl
chmod 0755 runtime/manager.py
systemctl daemon-reload
systemctl enable --now barista-gamepad-manager.service
for attempt in $(seq 1 40); do
    test ! -S /run/barista-gamepad/control.sock || exit 0
    sleep 0.25
done
systemctl status barista-gamepad-manager.service --no-pager
exit 1
