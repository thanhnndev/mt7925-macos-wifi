# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-4 raw disassembly (logger options, RegistrationInfo, queue/pool contract). Read-only.

Addresses are verified only for the recorded 24H32 collection: the script refuses any other hash.
Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round4.py <out-dir>
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

# file stem -> function start (each must be an exact symbol start in this KC)
FUNCS = {
    # CoreCapture logger construction
    'CCPipe-initWithOwnerNameCapacity': 0xffffff80031ba906,
    'CCLogPipe-initWithOwnerNameCapacity': 0xffffff80031cbe6e,
    # who builds the logger and the options
    'AppleBCMWLANCore-initDebugInfo': 0xffffff800159af70,
    'IO80211Controller-setupCPL': 0xffffff8002222eda,
    # interface registration chain
    'IO80211SkywalkInterface-start': 0xffffff8002261822,
    'IOSkywalkNetworkInterface-registerNetworkInterface-a': 0xffffff8002981d7e,
    'IOSkywalkNetworkInterface-registerNetworkInterface-b': 0xffffff8002981eac,
    'IOSkywalkEthernetInterface-initRegistrationInfo': 0xffffff8002987ed6,
    'IOSkywalkEthernetInterface-copyRegistrationInfo': 0xffffff8002987fe4,
    'IOSkywalkEthernetInterface-registerEthernetInterface-r4': 0xffffff8002988078,
    'IO80211InfraInterface-registerInfraEthernetInterface-r4': 0xffffff80022dcfce,
    'AppleBCMWLANCore-createSkywalkInterface': 0xffffff800163aaf8,
    'AppleBCMWLANSkywalkInterface-start-full': 0xffffff800156a576,
    'AppleBCMWLANSkywalkInterface-stop': 0xffffff800156ac34,
    'AppleBCMWLANSkywalkInterface-free': 0xffffff800156a1c2,
    'AppleBCMWLANSkywalkInterface-getTxQueueDepth': 0xffffff800156d422,
    'AppleBCMWLANSkywalkInterface-getRxQueueCapacity': 0xffffff800156d446,
    # bus layer: pool + queues
    'AppleBCMWLANBusInterfacePCIe-allocateSkywalkBusResource': 0xffffff800151cf22,
    'AppleBCMWLANPCIeSkywalk-allocSkywalkCommonResources': 0xffffff80014d93b0,
    'AppleBCMWLANPCIeSkywalk-allocSkywalkMonitorResources': 0xffffff80014d969a,
    'AppleBCMWLANPCIeSkywalk-registerSkywalkInterface': 0xffffff80014d9bae,
    'AppleBCMWLANPCIeSkywalk-free': 0xffffff80014d99f8,
    'AppleBCMWLANPCIeSkywalk-initWithProviderAndCapacity-r4': 0xffffff80014d925c,
    'AppleBCMWLANPCIeSkywalkPacketPool-withName-r4': 0xffffff80014d76aa,
    'AppleBCMWLANSkywalkPacketPool-initWithName': 0xffffff80016ebab0,
    'AppleBCMWLANPCIeSkywalkTxSubmissionQueue-withProviderAndQueueInfoAndPool': 0xffffff80014d15e6,
    'AppleBCMWLANPCIeSkywalkTxCompletionQueue-withProviderAndQueueInfoAndPool': 0xffffff80014d4f36,
    'AppleBCMWLANPCIeSkywalkRxSubmissionQueue-withProviderAndQueueInfoAndPool': 0xffffff80014d5854,
    'AppleBCMWLANPCIeSkywalkRxCompletionQueue-withProviderAndQueueInfoAndPool': 0xffffff80014d6a32,
    'AppleBCMWLANSkywalkTxSubmissionQueue-initWithPoolAndQueueInfo': 0xffffff800156873e,
    'AppleBCMWLANSkywalkTxCompletionQueue-initWithPoolAndQueueInfo': 0xffffff8001568dc4,
    'AppleBCMWLANSkywalkRxSubmissionQueue-initWithPoolAndQueueInfo': 0xffffff8001569196,
    'AppleBCMWLANSkywalkRxCompletionQueue-initWithPoolAndQueueInfo': 0xffffff80015695ae,
    # IOSkywalk base classes
    'IOSkywalkPacketBufferPool-initWithName-r4': 0xffffff800297cc58,
    'IOSkywalkPacketQueue-initWithPool': 0xffffff8002980860,
    'IOSkywalkTxSubmissionQueue-initWithPool': 0xffffff80029840e8,
    'IOSkywalkRxSubmissionQueue-initWithPool': 0xffffff80029857ee,
    'IOSkywalkTxCompletionQueue-initWithPool': 0xffffff8002989e96,
    'IOSkywalkRxCompletionQueue-initWithPool': 0xffffff800298648a,
}

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
for stem, addr in FUNCS.items():
    if addr not in kc.syms:
        sys.exit('%s: %#x is not a symbol start in this KC' % (stem, addr))
    with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        print('#', hex(addr), kc.syms[addr])
        kc.disasm(*kc.func_range(addr), comment_strings=True)
(out / 'source.json').write_text(
    '{\n  "sha256": "%s",\n  "tool": "tools/dump_native_wifi_round4.py",\n  "functions": %d\n}\n' % (EXPECTED, len(FUNCS)))
# small data blobs referenced by the code above (read straight from the collection)
with (out / 'data-tables.txt').open('w') as stream:
    def row(label, va, n):
        o = kc.va2off(va)
        stream.write('%s %#x %s\n' % (label, va, kc.d[o:o + n].hex()))
    # indexed by apple80211_wme_ac in AppleBCMWLANSkywalkTxSubmissionQueue::initWithPoolAndQueueInfo;
    # the value goes to IOSkywalkTxSubmissionQueue::initWithPool arg 3 (meaning: not determined)
    row('txsub_ac_table_u32x4', 0xffffff80016f6780, 16)
