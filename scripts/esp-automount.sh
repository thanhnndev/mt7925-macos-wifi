#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
# Mount the internal ESP at /Volumes/ESP owned by you at every login, so EFI/OC can be edited without sudo.
# At login a macOS password box appears (administrator rights for mount_msdos); type the password once per boot.
#   scripts/esp-automount.sh --install     copy the helper to ~/Library/Application Support and add a login LaunchAgent
#   scripts/esp-automount.sh --uninstall   remove both again (an ESP that is mounted stays mounted until reboot)
#   scripts/esp-automount.sh               mount now (what the LaunchAgent runs)
set -u
LABEL=com.itlmt7925.espmount
DIR="$HOME/Library/Application Support/EspAutomount"
HELPER="$DIR/esp-automount.sh"
AGENT="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/esp-automount.log"
MP=/Volumes/ESP

case "${1:-}" in
--install)
  mkdir -p "$DIR" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
  cp "$0" "$HELPER"; chmod 755 "$HELPER"
  /usr/bin/python3 -I - "$AGENT" "$LABEL" "$HELPER" "$LOG" <<'PY'
import plistlib, sys
path, label, helper, log = sys.argv[1:]
plistlib.dump({"Label": label, "ProgramArguments": ["/bin/bash", helper], "RunAtLoad": True,
               "StandardOutPath": log, "StandardErrorPath": log}, open(path, "wb"))
PY
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  launchctl bootstrap "gui/$(id -u)" "$AGENT" && echo "installed: $AGENT (runs at every login; mounting now, look for the password box)"
  exit 0;;
--uninstall)
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  rm -f "$AGENT" "$HELPER"; rmdir "$DIR" 2>/dev/null || true
  echo "removed $AGENT and $HELPER (log kept: $LOG)"
  exit 0;;
"") ;;
*) echo "usage: $0 [--install | --uninstall]"; exit 2;;
esac

echo "$(date '+%F %T') start"
if mount | grep -q " on $MP (msdos"; then echo "already mounted at $MP"; exit 0; fi
# the EFI partition of the internal physical disk (the one OpenCore boots from on this laptop)
DEV=$(diskutil list internal physical 2>/dev/null | awk '$2 == "EFI" {print $NF; exit}')
[ -n "$DEV" ] || { echo "no internal EFI partition found"; exit 1; }
if diskutil info "$DEV" | grep -q 'Mounted: *Yes'; then
  echo "$DEV is mounted elsewhere ($(diskutil info "$DEV" | awk -F': *' '/Mount Point/{print $2}')): unmounting it first"
  diskutil unmount "$DEV" >/dev/null || { echo "could not unmount $DEV"; exit 1; }
fi
U=$(id -u); G=$(id -g)
CMD="mkdir -p $MP && /sbin/mount_msdos -u $U -g $G -m 755 /dev/$DEV $MP"
if /usr/bin/osascript -e "do shell script \"$CMD\" with prompt \"Mount the EFI partition (OpenCore) at $MP for editing.\" with administrator privileges"; then
  echo "mounted /dev/$DEV at $MP (owner $(id -un))"
else
  echo "not mounted (password box cancelled or mount failed)"; exit 1
fi
