#!/bin/bash
# Removes everything the MacWifi package installed, including remembered networks.
# Run: sudo "/Library/Application Support/macwifi/uninstall.sh"   (or scripts/uninstall.sh from the repository)
set -u
if [ "$(id -u)" != 0 ]; then echo "Run it with sudo."; exit 1; fi
D="/Library/Application Support/macwifi"
user=$(stat -f %Su /dev/console 2>/dev/null)
if [ -n "$user" ] && [ "$user" != "root" ]; then
    launchctl bootout "gui/$(id -u "$user")/io.github.fpm8588.macwifi.menu" 2>/dev/null
fi
pkill -x MacWifi 2>/dev/null
launchctl bootout system/io.github.fpm8588.macwifid 2>/dev/null   # it removes its network service on the way out
sleep 1
[ -x "$D/macwifictl" ] && "$D/macwifictl" cleanup >/dev/null 2>&1
rm -f /Library/LaunchDaemons/io.github.fpm8588.macwifid.plist /Library/LaunchAgents/io.github.fpm8588.macwifi.menu.plist
[ -L /usr/local/bin/macwifictl ] && rm -f /usr/local/bin/macwifictl
if [ -d /Library/Audio/Plug-Ins/HAL/MacWifiAudio.driver ]; then
    rm -rf /Library/Audio/Plug-Ins/HAL/MacWifiAudio.driver
    launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod 2>/dev/null
fi
rm -rf /Applications/MacWifi.app "$D" /var/run/macwifid.sock /Library/Logs/macwifid.log /Library/Logs/macwifid.log.[0-9]
pkgutil --forget io.github.fpm8588.macwifi >/dev/null 2>&1
echo "MacWifi removed (service, menu bar app, remembered networks). Unplug the adapter to reset it."
