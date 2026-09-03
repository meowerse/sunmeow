#!/bin/sh

# User Service
# MEOW-TOUCH(rebrand): stop and remove OUR unit. The upstream name belongs to the distro
# package -- stopping that one is both ineffective here and hostile to a co-installed Sunshine.
systemctl --user stop app-meow.alxnko.sunmeow
rm "$HOME/.config/systemd/user/app-meow.alxnko.sunmeow.service"
systemctl --user daemon-reload
echo "Sunmeow User Service has been removed."

# Remove rules
flatpak-spawn --host pkexec sh -c "rm /etc/modules-load.d/60-sunmeow.conf"
flatpak-spawn --host pkexec sh -c "rm /etc/udev/rules.d/60-sunmeow.rules"
echo "Input rules removed. Restart computer to take effect."
