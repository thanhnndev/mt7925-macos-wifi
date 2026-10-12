# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Round-9 raw dump: who ends IODeferBSDAttach and publishes the primary Wi-Fi interface (read-only, no reboot needed).

Usage: KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round9.py <out-dir>
Writes: exact-name function dumps (deferBSDAttach, prepare/finalizeBSDInterface, BSD name/unit getters, registerNetworkInterface
overloads, EthernetInterface BSD helpers), the functions that reference the string "IODeferBSDAttach", and callsites-defer.txt: every
indirect call through vptr+0x9f8 (IOSkywalkNetworkInterface::deferBSDAttach) inside IO80211/AppleBCMWLAN/IOSkywalk functions with the preceding
instructions, so the caller that passes false can be found. Also callsites-registerservice.txt: indirect calls through vptr+0x5b0/0x5c0
(IOService::registerService region) inside IO80211/AppleBCMWLAN/IOSkywalk functions, to find who publishes the interface.
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
    '__ZN25IOSkywalkNetworkInterface14deferBSDAttachEb',
    '__ZN25IOSkywalkNetworkInterface19prepareBSDInterfaceEP7__ifnetj',
    '__ZN25IOSkywalkNetworkInterface20finalizeBSDInterfaceEP7__ifnetj',
    '__ZN25IOSkywalkNetworkInterface16getBSDNamePrefixEv',
    '__ZN25IOSkywalkNetworkInterface16getBSDUnitNumberEv',
    '__ZN25IOSkywalkNetworkInterface10setBSDNameEPKc',
    '__ZN25IOSkywalkNetworkInterface15setRunningStateEb',
    '__ZN25IOSkywalkNetworkInterface18setInterfaceEnableEb',
    '__ZN25IOSkywalkNetworkInterface39registerNetworkInterfaceWithLogicalLinkEPKNS_16RegistrationInfoEP20IOSkywalkLogicalLinkP25IOSkywalkPacketBufferPoolS6_j',
    '__ZN26IOSkywalkEthernetInterface26initBSDInterfaceParametersEP18ifnet_init_eparamsPP11sockaddr_dl',
    '__ZN26IOSkywalkEthernetInterface19prepareBSDInterfaceEP7__ifnetj',
    '__ZN26IOSkywalkEthernetInterface16getBSDNamePrefixEv',
    '__ZN25IOSkywalkNetworkInterface4stopEP9IOService',
    '__ZN25IOSkywalkNetworkInterface4initEP12OSDictionary',
]
by_name = {n: a for a, n in kc.syms.items()}
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
found = {}
missing = []


def dump(name, addr, tag=''):
    stem = re.sub(r'[^A-Za-z0-9]+', '_', name.lstrip('_'))[:110] + '_' + hex(addr)[-6:]
    with (out / (stem + '.asm.txt')).open('w') as stream, contextlib.redirect_stdout(stream):
        print('#', hex(addr), name, tag)
        kc.disasm(*kc.func_range(addr), comment_strings=True)
    found[name] = {'file': stem + '.asm.txt', 'address': hex(addr)}


for n in EXACT:
    if n in by_name:
        dump(n, by_name[n])
    else:
        missing.append(n)

# functions referencing the defer property string
strs = kc.find_string_va('IODeferBSDAttach')
refs = kc.xrefs(strs) if strs else {}
ref_funcs = {}
for tgt, sites in refs.items():
    for s in sites:
        f = kc.find_func_start(s)
        if f is not None:
            ref_funcs[f] = kc.syms.get(f, hex(f))
for f, n in sorted(ref_funcs.items())[:20]:
    dump(n, f, '(references "IODeferBSDAttach")')
(out / 'string-refs.txt').write_text('strings: %s\nfunctions: %s\n' % (
    [hex(s) for s in strs], [(hex(f), n) for f, n in sorted(ref_funcs.items())]))

# raw byte scan for indirect calls: ff /2 with disp32 (modrm 0x90-0x97 except SIB) and optional REX.B prefix
def scan(disps, keep, path, limit=600):
    pat = re.compile(rb'(?:\x41)?\xff[\x90-\x93\x95-\x97](' + b'|'.join(re.escape(x.to_bytes(4, 'little')) for x in disps) + b')')
    lines = []
    for name, vm, sz, fo, fs, pr in kc.segs:
        if not (pr & 4):
            continue
        seg = kc.d[fo:fo + fs]
        for m in pat.finditer(seg):
            va = vm + m.start()
            f = kc.find_func_start(va)
            fn = kc.syms.get(f, '?') if f is not None else '?'
            if not keep(fn):
                continue
            # window: decode from up to 40 bytes before, keep the last 6 instructions that end at the call
            ctx = []
            for back in range(40, 0, -1):
                ins = list(kc.md.disasm(kc.d[kc.va2off(va - back):kc.va2off(va - back) + back + 8], va - back))
                if ins and any(i.address == va for i in ins):
                    ctx = [i for i in ins if i.address <= va][-6:]
                    break
            lines.append('%x %s\n    %s' % (va, fn, '\n    '.join('%x: %s %s' % (i.address, i.mnemonic, i.op_str) for i in ctx)))
            if len(lines) >= limit:
                break
    Path(path).write_text('\n'.join(lines) + '\n')
    return len(lines)


want = re.compile(r'IO80211|AppleBCMWLAN|IOSkywalk|AppleMT|IOUserNetwork|Apple80211')
n1 = scan([0x9f8], lambda fn: bool(want.search(fn)), out / 'callsites-defer.txt')
n2 = scan([0x5b0, 0x5c0], lambda fn: bool(want.search(fn)), out / 'callsites-registerservice.txt')

(out / 'missing.txt').write_text('\n'.join(missing) + '\n')
(out / 'source.json').write_text(json.dumps({
    'sha256': EXPECTED, 'tool': 'tools/dump_native_wifi_round9.py', 'functions': found,
    'defer_callsites': n1, 'registerservice_callsites': n2,
}, indent=2) + '\n')
print('%d functions, %d defer call sites, %d registerService call sites, %d missing' % (len(found), n1, n2, len(missing)))
print('\n'.join(missing))
