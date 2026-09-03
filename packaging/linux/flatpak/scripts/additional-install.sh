#!/bin/sh

# User Service
mkdir -p ~/.config/systemd/user
# MEOW-TOUCH(rebrand): the unit is generated from PROJECT_FQDN, so the upstream name does not
# exist in the build -- this cp silently copied nothing and the service was never installed.
# The asset dir stays `share/sunshine` (SUNSHINE_ASSETS_DIR is deliberately unchanged).
cp "/app/share/sunshine/systemd/user/app-meow.alxnko.sunmeow.service" "$HOME/.config/systemd/user/app-meow.alxnko.sunmeow.service"
echo "Sunmeow User Service has been installed."
echo "Use [systemctl --user enable app-meow.alxnko.sunmeow] once to autostart Sunmeow on login."

# MEOW-TOUCH(rebrand): upstream's logic, our filenames. Upstream added the udevadm reload
# and the four triggers in c75d5d76 -- without them the new rules do not apply to devices
# that already exist, so gamepads stay unusable until a reboot. Only 60-sunshine.* ->
# 60-sunmeow.* differs; the asset dir stays share/sunshine (SUNSHINE_ASSETS_DIR unchanged).
# Load uhid for descriptor-driven gamepad emulation
UHID=$(cat /app/share/sunshine/modules-load.d/60-sunmeow.conf)
echo "Enabling gamepad emulation."
flatpak-spawn --host pkexec sh -c "echo '$UHID' > /etc/modules-load.d/60-sunmeow.conf"
flatpak-spawn --host pkexec modprobe uhid

# Udev rule
UDEV=$(cat /app/share/sunshine/udev/rules.d/60-sunmeow.rules)
echo "Configuring virtual input permissions."
flatpak-spawn --host pkexec sh -c "echo '$UDEV' > /etc/udev/rules.d/60-sunmeow.rules"
flatpak-spawn --host pkexec udevadm control --reload-rules
flatpak-spawn --host pkexec udevadm trigger --property-match=DEVNAME=/dev/uinput
flatpak-spawn --host pkexec udevadm trigger --property-match=DEVNAME=/dev/uhid
flatpak-spawn --host pkexec udevadm trigger --subsystem-match=hidraw
flatpak-spawn --host pkexec udevadm trigger --subsystem-match=input
echo "Virtual input permissions have been updated."
