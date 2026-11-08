#!/bin/sh
# Prints a diagnostics report to paste into a GitHub issue. MAC addresses (including your router's), IP
# addresses and network names are masked; nothing is sent anywhere. Run:
#   sh "/Library/Application Support/macwifi/report.sh"        (installed)    or    sh scripts/report.sh
D="/Library/Application Support/macwifi"
CTL="$D/macwifictl"; [ -x "$CTL" ] || CTL="$(dirname "$0")/../build/macwifictl"
LOG=/Library/Logs/macwifid.log
mask() {
    sed -E -e 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g' \
           -e 's/(^|[^0-9.])([0-9]{1,3}\.){3}[0-9]{1,3}([^0-9.]|$)/\1x.x.x.x\3/g' \
           -e 's/"ssid":"([^"\\]|\\.)*"/"ssid":"<name>"/g' \
           -e 's/(Looking for|password for|Join) "[^"]*"/\1 "<name>"/g'
}
echo '```'
echo "== system";   sw_vers 2>/dev/null; uname -m
echo "== macwifi";  "$CTL" --version 2>&1
echo "== firmware"; ls "$D/firmware" 2>&1
echo "== adapter";  "$CTL" list --all 2>&1 | mask
echo "== service";  launchctl print system/io.github.fpm8588.macwifid 2>/dev/null | grep -E "state =|pid =|last exit code" ; "$CTL" status 2>&1 | mask
echo "== log (last 150 lines)"; tail -150 "$LOG" 2>&1 | mask
echo '```'
