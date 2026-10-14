#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Offline symbol check of native-kext objects (no kernel collection needed): every undefined symbol that names an Apple
Family class (IO80211*, IOSkywalk*, CC*) must appear in the 24H32 method/vtable dumps under abi/native-wifi. What is not
in the dumps is listed so that `tools/check_kext_links.py` (needs the KC, run on the Mac) can settle it.

  python3 tools/check_native_symbols.py build/obj/itlmt7925Native_*.o
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAMILY = re.compile(r'^__Z[NK]*\d+(IO80211|IOSkywalk|CC)\w*')


def known():
    out = set()
    for p in glob.glob(os.path.join(ROOT, 'abi/native-wifi/24H32/**/*.methods.txt'), recursive=True) + \
            glob.glob(os.path.join(ROOT, 'abi/native-wifi/24H32/**/*.vtable.txt'), recursive=True):
        for line in open(p):
            f = line.rstrip('\n').split('\t')
            if f and f[-1].startswith('__Z'):
                out.add(f[-1])
    for p in glob.glob(os.path.join(ROOT, 'abi/native-wifi/24H32/**/*.asm.txt'), recursive=True):
        out.update(re.findall(r'; (__Z\w+)', open(p).read()))      # call targets named in the disassembly
    return out


def main():
    k = known()
    bad = []
    for obj in sys.argv[1:]:
        nm = subprocess.run(['llvm-nm', '-u', '--no-llvm-bc', obj], capture_output=True, text=True).stdout.split()
        for s in nm:
            if FAMILY.match(s) and s not in k:
                bad.append((obj, s))
    for obj, s in bad:
        print('not in the dumps: %s  (%s)' % (s, os.path.basename(obj)))
    print('%d family symbols outside the dumps' % len(bad))
    return 0


if __name__ == '__main__':
    sys.exit(main())
