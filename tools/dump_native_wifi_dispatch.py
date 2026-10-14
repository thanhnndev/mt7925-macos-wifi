# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Dump request tables verified in the 24H32 KC only. Set KC and pass output dir."""
import contextlib
import hashlib
from pathlib import Path
import struct
import sys
import kc

EXPECTED = 'cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3'
if hashlib.sha256(kc.d).hexdigest() != EXPECTED:
    sys.exit('Unsupported KC: table addresses are verified only for the recorded 24H32 hash')
if kc.md is None:
    sys.exit('Capstone required')
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
for mode, base in [('get', 0xffffff80023e7770), ('set', 0xffffff80023e8900)]:
    with (out / ('ioctl-' + mode + '-dispatch.txt')).open('w') as stream:
        stream.write('# apple80211RouteInternal table: request IDs 1..561, KC chained pointer low 30 bits\n')
        for request in range(1, 562):
            raw = struct.unpack_from('<Q', kc.d, kc.va2off(base) + (request - 1) * 8)[0]
            target = 0xffffff8000100000 + (raw & 0x3fffffff) if raw else 0
            name = kc.syms.get(target, '-' if not raw else hex(target))
            stream.write(f'{request}\t{request:#x}\t{target:#x}\t{name}\n')
            if any(term in name.upper() for term in ('SCAN', 'ASSOC', 'CAPABILIT')):
                with (out / f'ioctl-{mode}-{request}.asm.txt').open('w') as asm, contextlib.redirect_stdout(asm):
                    print('#', hex(target), name)
                    kc.disasm(*kc.func_range(target), comment_strings=False)
