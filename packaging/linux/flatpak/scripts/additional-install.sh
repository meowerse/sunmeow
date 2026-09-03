#!/bin/sh

# User Service
mkdir -p ~/.config/systemd/user
# MEOW-TOUCH(rebrand): the unit is generated from PROJECT_FQDN, so the upstream name does not
# exist in the build -- this cp silently copied nothing and the service was never installed.
# The asset dir stays `share/sunshine` (SUNSHINE_ASSETS_DIR is deliberately unchanged).
cp "/app/share/sunshine/systemd/user/app-meow.alxnko.sunmeow.service" "$HOME/.config/systemd/user/app-meow.alxnko.sunmeow.service"
echo "Sunmeow User Service has been installed."
echo "Use [systemctl --user enable app-meow.alxnko.sunmeow] once to autostart Sunmeow on login."

# Load uhid (DS5 emulation)
UHID=$(cat /app/share/sunshine/modules-load.d/60-sunmeow.conf)
echo "Enabling DS5 emulation."
flatpak-spawn --host pkexec sh -c "echo '$UHID' > /etc/modules-load.d/60-sunmeow.conf"
flatpak-spawn --host pkexec modprobe uhid

# Udev rule
UDEV=$(cat /app/share/sunshine/udev/rules.d/60-sunmeow.rules)
echo "Configuring mouse permission."
flatpak-spawn --host pkexec sh -c "echo '$UDEV' > /etc/udev/rules.d/60-sunmeow.rules"
echo "Restart computer for mouse permission to take effect."
