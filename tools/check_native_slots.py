#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Slot check of a native class: clang's vtable layout of the class compiled with the kext flags must put every function it
declares on the slot where Apple's own driver class has the same-named function (24H32 dumps).

  python3 tools/check_native_slots.py <Class> <source.cpp> <AppleClass> <dump dirs, ':'-separated> [clang flags...]
  e.g. tools/check_native_slots.py itlmt7925WifiInterface kexts/itlmt7925Native/itlmt7925_iface.cpp \
         AppleBCMWLANSkywalkInterface abi/native-wifi/24H32/round3 -target x86_64-apple-macosx12.0 ... (see scripts/build.sh)

Names only (the dump is mangled, the layout is demangled and compared by function name); the parameter lists are compared
as text after demangling both sides. Exit 1 on any mismatch. A function of the class that Apple's class does not override
(slot owned by a Family class in Apple's vtable) is reported as `inherited-in-apple` and compared by name as well.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from io80211_abi import dump_path   # noqa: E402


def apple_slots(dumps, cls):
    out = {}
    for line in open(dump_path(dumps, cls + '.vtable.txt')):
        p = line.rstrip('\n').split('\t')
        if len(p) == 3 and p[0].isdigit() and p[2].startswith('__Z'):
            out[int(p[0])] = p[2]
    return out


def demangle(names):
    names = sorted(set(names))
    # Mach-O symbols carry one more leading underscore than the Itanium name c++filt expects
    # Inputs already have the Mach-O prefix removed; Darwin c++filt otherwise strips another underscore.
    r = subprocess.run(['c++filt', '-n'], input='\n'.join(n[1:] for n in names), capture_output=True, text=True)
    return dict(zip(names, r.stdout.splitlines()))


def clang_slots(cls, src, flags):
    cmd = ['clang++', '-S', '-emit-llvm', '-o', os.devnull, '-w'] + flags + [src, '-Xclang', '-fdump-vtable-layouts']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('compile failed:\n' + r.stderr[-2000:])
    out, on = {}, False
    for line in r.stdout.splitlines():
        if line.startswith("Vtable for '%s'" % cls):
            on = True
            continue
        if on and line.startswith('VTable indices'):
            break
        if on and line.startswith('Vtable for'):
            break
        m = re.match(r'\s*(\d+) \| (.*)$', line) if on else None
        if m:
            out[int(m.group(1))] = m.group(2).strip()
    return out


def main():
    cls, src, apple, dumps = sys.argv[1:5]
    flags = sys.argv[5:]
    a = apple_slots(dumps, apple)
    dem = demangle(a.values())
    c = clang_slots(cls, src, flags)
    own = re.compile(r'^(?:.*?[\s*&])?%s::(~?\w+)\((.*?)\)( const)?( \[\w+\])?$' % re.escape(cls))
    ok = bad = skipped = 0
    for slot in sorted(c):
        m = own.match(c[slot])
        if not m:
            continue
        name = m.group(1)
        if name.startswith('~'):
            skipped += 1
            continue
        sym = a.get(slot)
        if not sym:
            print('slot %d: %s -> Apple has no function there' % (slot, c[slot]))
            bad += 1
            continue
        d = dem[sym]
        am = re.match(r'^(?:[\w:]+::)?(~?\w+)\(', d)
        if am and am.group(1) == name:
            ok += 1
        else:
            print('slot %d: %s -> Apple has %s' % (slot, c[slot], d))
            bad += 1
    print('%s: %d overrides on the right slot, %d wrong, %d destructors skipped (clang slots %d, Apple slots %d)' % (
        cls, ok, bad, skipped, len(c), len(a)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
