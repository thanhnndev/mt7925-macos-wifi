# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-5 raw dump (interface-side protocol classes, stage 2 of the native stub). Read-only.

Addresses are verified only for the recorded 24H32 collection: the script refuses any other hash.
Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round5.py <out-dir>
Writes vtable/method dumps of the protocol classes, their MetaClass constructors and the listed functions.
"""
import contextlib
import hashlib
from pathlib import Path
import sys
import kc
import kc_vtable as vt

EXPECTED = 'cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3'
if hashlib.sha256(kc.d).hexdigest() != EXPECTED:
    sys.exit('Unsupported KC: addresses are verified only for the recorded 24H32 hash')
if kc.md is None:
    sys.exit('Capstone required')

CLASSES = ['IO80211InfraProtocol', 'AppleBCMWLANInfraProtocol']

# file stem -> function start (each must be an exact symbol start in this KC)
FUNCS = {
    'AppleBCMWLANInfraProtocol-initWithProvider': 0xffffff800154f7f4,
    'AppleBCMWLANSkywalkInterface-init': 0xffffff8001569f00,
    'AppleBCMWLANSkywalkInterface-start-ioservice': 0xffffff800156a2b0,
    'AppleBCMWLANSkywalkInterface-getHardwareAssists': 0xffffff800156a2e6,
    'AppleBCMWLANSkywalkInterface-setMacAddress': 0xffffff800156c672,
    'AppleBCMWLANSkywalkInterface-setRunningState': 0xffffff800156b1d6,
    'IO80211InfraInterface-getHardwareAssists': 0xffffff80022dc2a6,
}

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
summary = []
for cls in CLASSES:
    summary.append('%s %d' % (cls, len(vt.vtable(cls))))
    with (out / (cls + '.vtable.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        vt.cmd_vtable(cls)
    with (out / (cls + '.methods.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        vt.cmd_methods(cls)
    ctors = [(a, n) for a, n in sorted(kc.syms.items()) if n.startswith('__ZN%d%s9MetaClassC' % (len(cls), cls))]
    with (out / (cls + '-MetaClass.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        for a, n in ctors:
            print('#', hex(a), n)
            kc.disasm(*kc.func_range(a), comment_strings=False)
for stem, addr in FUNCS.items():
    if addr not in kc.syms:
        sys.exit('%s: %#x is not a symbol start in this KC' % (stem, addr))
    with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        print('#', hex(addr), kc.syms[addr])
        kc.disasm(*kc.func_range(addr), comment_strings=True)
(out / 'source.json').write_text(
    '{\n  "sha256": "%s",\n  "tool": "tools/dump_native_wifi_round5.py",\n  "classes": %d,\n  "functions": %d\n}\n' % (
        EXPECTED, len(CLASSES), len(FUNCS)))
print('\n'.join(summary))
