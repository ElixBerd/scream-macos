#!/bin/sh
# Removes Scream: the menu bar app, the Scream Audio driver and the
# command line tools. Installed as /usr/local/bin/scream-uninstall so it
# still works after Scream.app has been dragged to the Trash.
set -e
if [ "$(id -u)" != 0 ]; then exec sudo "$0" "$@"; fi

[ "$1" = "--from-app" ] || pkill -x Scream 2>/dev/null || true
rm -rf /Applications/Scream.app /Library/Audio/Plug-Ins/HAL/ScreamAudio.driver
rm -f /usr/local/bin/screamctl /usr/local/bin/scream-uninstall

user=$(stat -f%Su /dev/console)
if [ -n "$user" ] && [ "$user" != "root" ]; then
  sudo -u "$user" defaults delete com.github.elixberd.Scream >/dev/null 2>&1 || true
fi
pkgutil --forget com.github.elixberd.scream >/dev/null 2>&1 || true

# Core Audio drops the device and falls back to the speakers.
killall coreaudiod 2>/dev/null || true
echo "Scream removed."
