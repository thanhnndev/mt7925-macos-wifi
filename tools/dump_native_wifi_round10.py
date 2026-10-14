# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-10 raw dump: the "trying to send on thread panic" at IO80211Glue.cpp:417 during BSD attach (read-only, no reboot needed).

Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round10.py <out-dir>
Panic backtrace (build 06840d9): IOSkywalkNetworkBSDClient::start -> kern_nexus_controller_alloc_net_provider_instance ->
prepareNexusCallback -> IOCommandGate::runAction -> gatedPrepareNexus -> IO80211InfraInterface::prepareBSDInterface ->
updateStaticProperties -> IO80211Glue::sendIOUCToWcl, which panics unless the work loop object at [glue ivars+0x38] reports
inGate() and !onThread(). Questions answered by these dumps: which work loop the BSD client's command gate is attached to, what the
Glue keeps at ivars+0x38, which ioctls updateStaticProperties sends, and which getWorkLoop overrides exist.
Writes: every function of the classes IO80211Glue and IOSkywalkNetworkBSDClient / IOSkywalkBSDClient (size capped), the exact
InfraInterface/Controller functions, and every getWorkLoop symbol of IO80211*/IOSkywalk* classes.
"""
import contextlib
import hashlib
import json
from pathlib import Path
import re
import sys
import kc

EXPECTED = 'cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3'
if hashlib.sha256(kc.d).hexdigest() != EXPECTED:
    sys.exit('Unsupported KC: verified only for the recorded 24H32 hash')
if kc.md is None:
    sys.exit('Capstone required')

EXACT = [
    '__ZN21IO80211InfraInterface22updateStaticPropertiesEv',
    '__ZN21IO80211InfraInterface19prepareBSDInterfaceEP7__ifnetj',
    '__ZN21IO80211InfraInterface20finalizeBSDInterfaceEP7__ifnetj',
    '__ZN23IO80211SkywalkInterface10getWorkLoopEv',
    '__ZNK23IO80211SkywalkInterface10getWorkLoopEv',
    '__ZNK17IO80211Controller10getWorkLoopEv',
    '__ZN17IO80211Controller10getWorkLoopEv',
    '__ZN11IO80211Glue15initWithOptionsEP23IO80211SkywalkInterface',
    '__ZN11IO80211Glue11withOptionsEP23IO80211SkywalkInterface',
    '__ZN11IO80211Glue22runActionBlockOnSerialEbU13block_pointerFivE',
    '__ZN11IO80211Glue9sendEventEjPvmRb',
    '__ZN11IO80211Glue9printTaskInfoEPKcyy',
    '__ZN11IO80211Glue22receiveMessageInternalER17CommonGlueMessage',
]
CLASS_RE = re.compile(r'^__ZN(?:25IOSkywalkNetworkBSDClient|18IOSkywalkBSDClient|35IOSkywalkNetworkNexusDomainProvider)\d')
WORKLOOP_RE = re.compile(r'^__ZN?K?\d+(?:IO80211|IOSkywalk|AppleBCMWLAN)\w*\d+getWorkLoop')
by_name = {n: a for a, n in kc.syms.items()}
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
found = {}
missing = []


def dump(name, addr, cap=6000):
    stem = re.sub(r'[^A-Za-z0-9]+', '_', name.lstrip('_'))[:110] + '_' + hex(addr)[-6:]
    lo, hi = kc.func_range(addr)
    if hi - lo > cap * 8:
        found[name] = {'file': None, 'address': hex(addr), 'skipped': 'size %d' % (hi - lo)}
        return
    with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        print('#', hex(addr), name)
        kc.disasm(lo, hi, comment_strings=True)
    found[name] = {'file': stem + '.asm.txt', 'address': hex(addr)}


for n in EXACT:
    if n in by_name:
        dump(n, by_name[n])
    else:
        missing.append(n)
for a, n in sorted(kc.syms.items()):
    if n not in found and (CLASS_RE.match(n) or WORKLOOP_RE.match(n)) and 'MetaClass' not in n:
        dump(n, a)

(out / 'missing.txt').write_text('\n'.join(missing) + '\n')
(out / 'source.json').write_text(json.dumps({'sha256': EXPECTED, 'tool': 'tools/dump_native_wifi_round10.py', 'functions': found}, indent=2) + '\n')
print('%d functions, %d missing' % (len(found), len(missing)))
print('\n'.join(missing))
