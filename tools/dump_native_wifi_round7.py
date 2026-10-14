# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-7 raw dump: controller work-loop/queue getters and queue factory (read-only).

Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round7.py <out-dir>
Includes getThread to verify the NULL-thread finding from the first Stage A panic.
"""
import contextlib
import hashlib
import json
from pathlib import Path
import sys
import kc

EXPECTED = 'cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3'
if hashlib.sha256(kc.d).hexdigest() != EXPECTED:
    sys.exit('Unsupported KC: verified only for the recorded 24H32 hash')
if kc.md is None:
    sys.exit('Capstone required')

NAMES = {
    'IO80211Controller-getWorkLoop': '__ZNK17IO80211Controller11getWorkLoopEv',
    'IO80211Controller-getWorkQueue': '__ZNK17IO80211Controller12getWorkQueueEv',
    'AppleBCMWLANCore-getWorkQueue': '__ZNK16AppleBCMWLANCore12getWorkQueueEv',
    'IO80211WorkQueue-workQueue': '__ZN16IO80211WorkQueue9workQueueEv',
    'IO80211WorkQueue-getThread': '__ZNK16IO80211WorkQueue9getThreadEv',
    'IOService-getWorkLoop': '__ZNK9IOService11getWorkLoopEv',
}
by_name = {n: a for a, n in kc.syms.items()}
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
missing = []
found = {}
for stem, sym in NAMES.items():
    addr = by_name.get(sym)
    if addr is None:
        missing.append('%s: %s not found' % (stem, sym))
        continue
    found[stem] = {'symbol': sym, 'address': hex(addr)}
    with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        print('#', hex(addr), sym)
        kc.disasm(*kc.func_range(addr), comment_strings=True)
(out / 'missing.txt').write_text('\n'.join(missing) + '\n')
(out / 'source.json').write_text(json.dumps({
    'sha256': EXPECTED, 'tool': 'tools/dump_native_wifi_round7.py', 'functions': found,
}, indent=2) + '\n')
print('\n'.join(missing) or 'all functions found')
sys.exit(1 if missing else 0)
