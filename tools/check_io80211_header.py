# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Check the generated IO80211Controller header against the vtable dump: clang's own vtable layout of a concrete subclass must put
the same `Class::method` at every slot as Apple's vtable (slots 2..464, i.e. the whole inherited IOKit part too).

  python3 tools/check_io80211_header.py abi/native-wifi/24H32/round3 [path/to/MacKernelSDK/Headers]
  python3 tools/check_io80211_header.py abi/native-wifi/24H32/round3 [SDK] --all | --class IO80211WorkQueue ...

Without --class/--all it checks IO80211Controller (slots 2..464). With them it checks the interface-side classes of
tools/io80211_abi.py, every slot against the class's own dump.
Needs clang++ and c++filt. Exit status 0 = layout identical.
"""
import os, re, subprocess, sys, tempfile

argv = [x for x in sys.argv[1:]]
want = []
while '--class' in argv:
    i = argv.index('--class')
    want.append(argv[i + 1])
    del argv[i:i + 2]
if '--all' in argv:
    argv.remove('--all')
    want = ['all']
D = argv[0]
SDK = argv[1] if len(argv) > 1 else os.environ.get('MACKERNELSDK', '/tmp/claude-0/sdk/MacKernelSDK/Headers')
here = os.path.dirname(os.path.abspath(__file__))
if want:
    sys.path.insert(0, here)
    import io80211_check
    sys.exit(io80211_check.check_main(D, SDK, want))
gen = subprocess.run([sys.executable, os.path.join(here, 'gen_io80211_header.py'), D], capture_output=True, text=True, check=True).stdout
tmp = tempfile.mkdtemp()
open(os.path.join(tmp, 'IO80211Controller.h'), 'w').write(gen)

apple = {}
for line in open(os.path.join(D, 'IO80211Controller.vtable.txt')):
    p = line.rstrip('\n').split('\t')
    if len(p) == 3 and p[0].isdigit():
        apple[int(p[0])] = p[2]
bcm = {}
for line in open(os.path.join(D, 'AppleBCMWLANCore.vtable.txt')):
    p = line.rstrip('\n').split('\t')
    if len(p) == 3 and p[0].isdigit():
        bcm[int(p[0])] = p[2]


def dem(sym):
    return subprocess.run(['c++filt', '-n', sym[1:]], capture_output=True, text=True).stdout.strip()


# a concrete subclass that overrides every pure slot of the generated header
overrides = []
for slot, sym in apple.items():
    if slot >= 396 and sym == '___cxa_pure_virtual':
        m = re.match(r'\w+::(\w+)\((.*)\)( const)?$', dem(bcm[slot]))
        overrides.append((slot, m.group(1)))
probe = ['#include "IO80211Controller.h"', 'class Probe : public IO80211Controller { public:', '    Probe() : IO80211Controller((const OSMetaClass *)0) {}']
for line in gen.splitlines():
    m = re.match(r'\s*/\* (\d+) \*/ virtual (.+?) (\w+)\((.*)\)( const)? = 0;', line)
    if m:
        probe.append('    %s %s(%s)%s override { return {}; }' % (m.group(2), m.group(3), re.sub(r' a\d+(,|$)', r'\1', m.group(4)), m.group(5) or ''))
probe.append('};\nvoid make_probe() { Probe p; }')
open(os.path.join(tmp, 'probe.cpp'), 'w').write('\n'.join(probe) + '\n')

cmd = ['clang++', '-S', '-emit-llvm', '-o', os.devnull, '-target', 'x86_64-apple-macosx12.0', '-nostdinc', '-nostdinc++', '-std=c++17',
       '-fno-exceptions', '-fno-rtti', '-ffreestanding', '-DKERNEL', '-DKERNEL_PRIVATE', '-DDRIVER_PRIVATE', '-DAPPLE', '-DNeXT',
       '-I' + SDK, '-I' + tmp, os.path.join(tmp, 'probe.cpp'), '-Xclang', '-fdump-vtable-layouts']
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[:4000]); sys.exit('compile failed (probe kept in %s)' % tmp)
mine, grab = {}, False
for line in r.stdout.splitlines():
    if line.startswith("Vtable for 'Probe'"):
        grab = True
        continue
    if grab:
        if not line.strip():
            break
        m = re.match(r'\s*(\d+) \| (.*)$', line)
        if m:
            mine[int(m.group(1))] = m.group(2)


def key_apple(sym):
    if sym in ('-', ''):
        return '-'
    d = dem(sym)
    d = re.sub(r'\(.*$', '', d)
    return d


def key_mine(txt):
    if 'RTTI' in txt or 'offset_to_top' in txt:
        return '-'
    m = re.search(r'([~\w]+::[~\w]+)\(', txt)
    return m.group(1) if m else txt.strip()


bad = 0
notes = []
last = max(apple)
for slot in range(2, last):
    a = key_apple(apple[slot])
    b = key_mine(mine.get(slot, '(missing)'))
    pure = apple[slot] == '___cxa_pure_virtual' and slot >= 396
    if pure:
        a = 'Probe::' + re.sub(r'.*::', '', key_apple(bcm[slot]))
    # destructors: Apple lists D1 then D0; clang lists [complete] then [deleting] under the final overrider's class
    a2, b2 = re.sub(r'^\w+::', '', a), re.sub(r'^\w+::', '', b)
    if slot in (2, 3):
        continue                      # destructors: D1/D0 vs complete/deleting
    # compare the method name; the class may differ when Probe overrides a pure slot
    if a2 != b2 and a != b and re.search(r'::_RESERVED', b) and slot < 396:
        notes.append((slot, a, b))      # Apple's header names a method the SDK still calls _RESERVED: same slot, fine
        continue
    if a2 != b2 and a != b:
        bad += 1
        if bad <= 25:
            print('slot %d: apple %-60s clang %s' % (slot, a, b))
for n in notes:
    print('note slot %d: Apple names it %s, SDK header calls it %s' % n)
print('slots compared: %d, mismatches: %d' % (last - 2, bad))
sys.exit(1 if bad else 0)
