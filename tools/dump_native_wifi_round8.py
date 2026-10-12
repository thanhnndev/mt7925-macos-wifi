# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-8 raw dump: why registerInfraEthernetInterface leaves no published BSD interface (read-only).

Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round8.py <out-dir>
Dumps the Skywalk queue-set / logical-link validation that registerNetworkInterface calls, the Tx queue factories (service
class argument), copyRegistrationInfo, and the start/publish path of IOSkywalkNetworkInterface / IOSkywalkInterface.
Symbols are matched by exact name when known, otherwise by substring (every match is dumped, at most MAXN per pattern).
"""
import contextlib
import hashlib
import json
from pathlib import Path
import re
import sys
import kc

EXPECTED = 'cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3'
MAXN = 6
if hashlib.sha256(kc.d).hexdigest() != EXPECTED:
    sys.exit('Unsupported KC: verified only for the recorded 24H32 hash')
if kc.md is None:
    sys.exit('Capstone required')

PATTERNS = [
    '__ZN17IOSkywalkQueueSet22isWMMSchedModelCapable',
    '__ZN17IOSkywalkQueueSet16createWithQueues',
    '__ZN17IOSkywalkQueueSet',            # all other QueueSet members: getQueueSetNexusInfo, init...
    '__ZN20IOSkywalkLogicalLink18createWithQueueSet',
    '__ZN20IOSkywalkLogicalLink23getLogicalLinkNexusInfo',
    '__ZN26IOSkywalkEthernetInterface20copyRegistrationInfo',
    '__ZN26IOSkywalkEthernetInterface16getBSDNamePrefix',
    '__ZN26IOSkywalkEthernetInterface27deregisterEthernetInterface',
    '__ZN26IOSkywalkTxSubmissionQueue8withPool',
    '__ZN26IOSkywalkTxSubmissionQueue8initWithPool',
    '__ZN25IOSkywalkNetworkInterface5start',
    '__ZN25IOSkywalkNetworkInterface4init',
    '__ZN25IOSkywalkNetworkInterface13registerInterface',
    '__ZN25IOSkywalkNetworkInterface18initBSDInterface',
    '__ZN25IOSkywalkNetworkInterface20setInterfaceFeatures',
    '__ZN18IOSkywalkInterface5start',
    '__ZN18IOSkywalkInterface8register',
    '__ZN18IOSkywalkInterface4init',
    '__ZN18IOSkywalkInterface12registerNexus',
]
by_name = {n: a for a, n in kc.syms.items()}
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
found = {}
missing = []
for pat in PATTERNS:
    hits = sorted((n, a) for n, a in by_name.items() if n.startswith(pat))
    if not hits:
        missing.append('%s: no symbol' % pat)
        continue
    for n, a in hits[:MAXN]:
        if n in found:
            continue
        stem = re.sub(r'[^A-Za-z0-9]+', '_', n.lstrip('_'))[:120] + '_' + hex(a)[-6:]
        found[n] = {'file': stem + '.asm.txt', 'address': hex(a)}
        with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
            print('#', hex(a), n)
            kc.disasm(*kc.func_range(a), comment_strings=True)
(out / 'missing.txt').write_text('\n'.join(missing) + '\n')
(out / 'source.json').write_text(json.dumps({
    'sha256': EXPECTED, 'tool': 'tools/dump_native_wifi_round8.py', 'functions': found,
}, indent=2) + '\n')
print('%d functions, %d patterns without match' % (len(found), len(missing)))
print('\n'.join(missing))
