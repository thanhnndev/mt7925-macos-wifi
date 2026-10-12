#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
# Backup / install / restore helper for the first load of itlmt7925Native (docs/TEST-NATIVE-WIFI.md). macOS only.
#   scripts/native-wifi-test.sh backup            copy ESP:/EFI to ~/itlmt-backup/EFI-<time> (+ tar.gz) and print the path
#   scripts/native-wifi-test.sh install <kext> [--stage a|b] [--exclusive]
#                                                 backup first, then copy the kext to OC/Kexts and add the Kernel/Add entry + boot-args.
#                                                 --stage a (default) = controller only (itlmt_native=1); b = + interface (itlmt_native_if=1).
#                                                 --exclusive = switch the old Wi-Fi off for this test: removes itlmt_m2/itlmt_m3/itlmt_fast
#                                                 (the core still loads and, with itlmt_m1, loads the firmware, but never scans/joins and
#                                                 creates no Ethernet interface), so only one of the two paths is live. restore undoes it.
#   scripts/native-wifi-test.sh restore [dir]     put the newest (or given) backup back, replacing ESP:/EFI
#   scripts/native-wifi-test.sh logs              collect kextstat/ioreg/log of the last boot into ~/itlmt-backup/logs-<time>
# ESP is taken from /Volumes/ESP (scripts/esp-automount.sh) or from ESP=/path.
set -eu
ESP=${ESP:-/Volumes/ESP}
BK=$HOME/itlmt-backup
TS=$(date +%Y%m%d-%H%M%S)
[ -d "$ESP/EFI/OC" ] || { echo "no $ESP/EFI/OC: mount the ESP first (scripts/esp-automount.sh or diskutil mount)"; exit 1; }
mkdir -p "$BK"

backup() {
  d=$BK/EFI-$TS
  cp -Rp "$ESP/EFI" "$d"
  tar -czf "$d.tar.gz" -C "$BK" "EFI-$TS"
  # NVRAM boot-args as well: it overrides config.plist when set
  nvram boot-args > "$d.nvram-boot-args.txt" 2>&1 || true
  echo "$d"
}

case "${1:-}" in
backup) backup;;
install)
  k=${2:?path to itlmt7925Native.kext}
  shift 2
  stage=a; excl=0
  while [ $# -gt 0 ]; do
    case "$1" in
    --stage) stage=${2:?--stage a|b}; shift 2;;
    --exclusive) excl=1; shift;;
    *) echo "unknown option $1"; exit 2;;
    esac
  done
  case "$stage" in a|b) ;; *) echo "--stage must be a or b"; exit 2;; esac
  [ -f "$k/Contents/Info.plist" ] || { echo "not a kext: $k"; exit 1; }
  d=$(backup); echo "backup: $d"
  # The core kext must be a library kext (OSBundleCompatibleVersion) or the native kext fails with 0xdc00800e. Patch the ESP copy
  # in place (keeps the injected Wi-Fi credentials; the binary is unchanged).
  /usr/bin/python3 -I - "$ESP/EFI/OC/Kexts/itlmt7925.kext/Contents/Info.plist" <<'PY'
import plistlib, sys
p = sys.argv[1]
c = plistlib.load(open(p, 'rb'))
if 'OSBundleCompatibleVersion' not in c:
    c['OSBundleCompatibleVersion'] = c['CFBundleVersion']
    plistlib.dump(c, open(p, 'wb'))
    print('core Info.plist: added OSBundleCompatibleVersion', c['CFBundleVersion'])
PY
  rm -rf "$ESP/EFI/OC/Kexts/itlmt7925Native.kext"
  cp -Rp "$k" "$ESP/EFI/OC/Kexts/"
  /usr/bin/python3 -I - "$ESP/EFI/OC/config.plist" "$stage" "$excl" <<'PY'
import plistlib, sys
p, stage, excl = sys.argv[1], sys.argv[2], sys.argv[3] == '1'
c = plistlib.load(open(p, 'rb'))
add = c['Kernel']['Add']
names = [a['BundlePath'] for a in add]
if 'itlmt7925Native.kext' not in names:
    core = next(i for i, a in enumerate(add) if a['BundlePath'] == 'itlmt7925.kext')
    e = {'Arch': 'Any', 'BundlePath': 'itlmt7925Native.kext', 'Comment': 'native Wi-Fi test', 'Enabled': True,
         'ExecutablePath': 'Contents/MacOS/itlmt7925Native', 'MaxKernel': '', 'MinKernel': '', 'PlistPath': 'Contents/Info.plist'}
    add.insert(core + 1, e)       # must come after the core kext it depends on
a = c['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']
toks = a.get('boot-args', '').split()
want = ['itlmt_native=1'] + (['itlmt_native_if=1'] if stage == 'b' else [])
drop = ['itlmt_native_if=1'] if stage == 'a' else []
if excl:
    drop += [t for t in toks if t.split('=')[0] in ('itlmt_m2', 'itlmt_m3', 'itlmt_fast')]
toks = [t for t in toks if t not in drop] + [t for t in want if t not in toks]
a['boot-args'] = ' '.join(toks)
plistlib.dump(c, open(p, 'wb'))
print('config.plist: Add entry after itlmt7925.kext, boot-args =', a['boot-args'])
PY
  echo "NVRAM boot-args (wins over config.plist if set; clear it or reset NVRAM from the picker if it differs):"
  nvram boot-args 2>&1 || true
  echo "done. Reboot. To go back: scripts/native-wifi-test.sh restore";;
restore)
  d=${2:-$(ls -d "$BK"/EFI-* 2>/dev/null | grep -v '\.' | tail -1)}
  [ -d "$d" ] || { echo "no backup directory"; exit 1; }
  rm -rf "$ESP/EFI"; cp -Rp "$d" "$ESP/EFI"
  echo "restored $d -> $ESP/EFI; reboot. Check: nvram boot-args (reset NVRAM from the OC picker if the native args are stuck)";;
logs)
  o=$BK/logs-$TS; mkdir -p "$o"
  kextstat 2>/dev/null | grep -i itlmt > "$o/kextstat.txt" || true
  ioreg -l -w0 | grep -i -B2 -A12 'itlmt7925' > "$o/ioreg.txt" || true
  log show --last boot --predicate 'eventMessage CONTAINS[c] "itlmt" OR process == "kernel"' --style compact > "$o/log.txt" 2>&1 || true
  dmesg | grep -i itlmt > "$o/dmesg.txt" || true
  ioreg -l -w0 | grep -E 'itlmt_native_trace|IOInterfaceNamePrefix|IOInterfaceUnit|IO80211InterfaceRole' > "$o/native-trace.txt" || true
  ioreg -r -c itlmt7925WifiInterface -l -w0 > "$o/native-interface-full.txt" 2>&1 || true
  ioreg -r -c itlmt7925Wifi -d1 -l -w0 | grep -v -i 'mac' > "$o/native-controller.txt" 2>&1 || true
  nvram boot-args > "$o/boot-args.txt" || true
  ifconfig -a | grep -E '^[a-z]+[0-9]+:' > "$o/interfaces.txt"
  echo "$o (check for SSIDs/MACs before sharing)";;
*) echo "usage: $0 backup | install <kext> [--stage a|b] [--exclusive] | restore [dir] | logs"; exit 2;;
esac
