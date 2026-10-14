# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-6 raw dump (first-load panic, stage A). Read-only.

The panic: page fault CR2=0x648 at IOLockLock+0x3 called from IONetworkController::start+0x1d4 (via IO80211Controller::start+0x41),
i.e. IONetworkController::start locks a lock at NULL+0x648. Which pointer is NULL (a field set by init, the provider, a global)
is not visible without the function, so this dumps it and its relatives by symbol name.
Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round6.py <out-dir>
"""
import contextlib
import hashlib
from pathlib import Path
import sys
import kc

EXPECTED = 'cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3'
if hashlib.sha256(kc.d).hexdigest() != EXPECTED:
    sys.exit('Unsupported KC: addresses are verified only for the recorded 24H32 hash')
if kc.md is None:
    sys.exit('Capstone required')

# file stem -> mangled symbol (Mach-O, one leading underscore more than Itanium); every one is looked up by name
NAMES = {
    'IONetworkController-start': '__ZN19IONetworkController5startEP9IOService',
    'IONetworkController-init': '__ZN19IONetworkController4initEP12OSDictionary',
    'IONetworkController-initialize': '__ZN19IONetworkController10initializeEv',
    'IOEthernetController-start': '__ZN18IOEthernetController5startEP9IOService',
    'IO80211Controller-init': '__ZN17IO80211Controller4initEP12OSDictionary',
    'AppleBCMWLANCore-start': '__ZN16AppleBCMWLANCore5startEP9IOService',
    'AppleBCMWLANCore-init': '__ZN16AppleBCMWLANCore4initEP12OSDictionary',
    'AppleBCMWLANCore-initWithProvider': '__ZN16AppleBCMWLANCore16initWithProviderEP9IOService',
}
by_name = {n: a for a, n in kc.syms.items()}
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
missing = []
for stem, sym in NAMES.items():
    addr = by_name.get(sym)
    if addr is None:
        # a near miss helps the next round: list symbols with the same class and method name
        part = sym.split('EP')[0]
        missing.append('%s: %s not found; similar: %s' % (stem, sym, ', '.join(sorted(n for n in by_name if n.startswith(part))[:6])))
        continue
    with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        print('#', hex(addr), sym)
        kc.disasm(*kc.func_range(addr), comment_strings=True)
(out / 'missing.txt').write_text('\n'.join(missing) + '\n')
print('\n'.join(missing) or 'all functions found')
