# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Layout check of the generated interface-side headers (see io80211_abi.py): clang's own vtable layout of a probe subclass
must put the same method name at every slot as Apple's vtable of that class (dump of one macOS build).

Used by `tools/check_io80211_header.py <dump-dir> [SDK] --class NAME | --all`.
"""
import os
import re
import subprocess
import tempfile

from io80211_abi import (CLASSES, ENUMS, ORDER, class_vt, declarator, demangle_all, dump_path, parse_method, render,
                         sdk_virtual_rets, vt)

# by-value parameters of virtuals: complete dummy definitions, only so that the probe can override a pure slot
BYVALUE_STUBS = ['IO80211FlowQueueHash', 'apple80211_channel', 'nicproxy_limits_info_s']


def _flags(sdk, tmp):
    return ['clang++', '-S', '-emit-llvm', '-o', os.devnull, '-target', 'x86_64-apple-macosx12.0', '-nostdinc', '-nostdinc++',
            '-std=c++17', '-fno-exceptions', '-fno-rtti', '-ffreestanding', '-DKERNEL', '-DKERNEL_PRIVATE', '-DDRIVER_PRIVATE',
            '-DAPPLE', '-DNeXT', '-w', '-I' + sdk, '-I' + tmp]


def check_class(d, sdk, cls, tmp, texts):
    conf = CLASSES[cls]
    mine = class_vt(d, cls)
    last = max(s for s, v in mine.items() if v != '-')
    kids = [vt(d, c) for c in conf['children'] if os.path.exists(dump_path(d, c + '.vtable.txt'))]
    dem = demangle_all({v for v in mine.values() if v.startswith('__Z')} |
                       {k[s] for k in kids for s in k if k[s].startswith('__Z')})
    sdkret = sdk_virtual_rets(sdk)
    gen_text = '\n'.join(texts.values())

    def ret_of(name):
        m = re.findall(r'virtual ([\w:\s\*&]+?) \*?%s\(' % re.escape(name), gen_text)
        if m:
            return m[0]
        c = sdkret.get(name)
        return next(iter(c)) if c and len(c) == 1 else None

    probe = ['#include <stdint.h>']
    for t in BYVALUE_STUBS:
        probe.append('struct %s { uint64_t pad[3]; };' % t)
    for e in sorted(ENUMS):
        if e not in ('IO80211CountryCodeOp', 'IO80211FeatureCode'):
            probe.append('#define IO80211_ENUM_%s' % e)
            probe.append('enum %s : uint32_t {};' % e)
    probe += ['#include "%s.h"' % cls, 'class Probe : public %s { public:' % cls,
              '    Probe() : %s((const OSMetaClass *)0) {}' % cls]
    unnamed = []
    for s in range(2, last + 1):
        if mine[s] != '___cxa_pure_virtual':
            continue
        sym = next((k[s] for k in kids if s in k and k[s] not in ('___cxa_pure_virtual', '-')), None)
        if sym is None:
            unnamed.append(s)
            continue
        owner, name, params, const = parse_method(dem[sym])
        plist = [declarator(t, i + 1) for i, t in enumerate(params)]
        ret = ret_of(name) or 'void'
        body = '{ return {}; }' if ret != 'void' else '{}'
        probe.append('    %s %s(%s)%s override %s' % (ret, name, ', '.join(plist), ' const' if const else '', body))
    probe.append('};\nvoid make_probe() { Probe p; }')
    open(os.path.join(tmp, 'probe.cpp'), 'w').write('\n'.join(probe) + '\n')
    r = subprocess.run(_flags(sdk, tmp) + [os.path.join(tmp, 'probe.cpp'), '-Xclang', '-fdump-vtable-layouts'],
                       capture_output=True, text=True)
    if r.returncode:
        return dict(ok=False, msg='compile failed: ' + r.stderr.strip()[:1800], unnamed=unnamed)
    got, grab = {}, False
    for line in r.stdout.splitlines():
        if line.startswith("Vtable for 'Probe'"):
            grab = True
            continue
        if grab:
            if not line.strip():
                break
            m = re.match(r'\s*(\d+) \| (.*)$', line)
            if m:
                got[int(m.group(1))] = m.group(2)
    bad, owner_diff, compared = [], [], 0
    for s in range(2, last + 1):
        sym = mine[s]
        if sym == '___cxa_pure_virtual' or s in (2, 3):
            continue                       # pure slots are named by the probe; destructors: D1/D0 vs complete/deleting
        compared += 1
        if not sym.startswith('__Z'):
            continue
        pm = parse_method(dem[sym])
        if not pm:
            continue
        a_owner, a_name, a_params, a_const = pm
        txt = got.get(s, '(missing)')
        m = re.search(r'([~\w]+)::([~\w]+)\((.*)\)', txt)
        if not m:
            bad.append((s, '%s::%s' % (a_owner, a_name), txt))
            continue
        b_owner, b_name, b_args = m.groups()
        if a_name != b_name:
            bad.append((s, '%s::%s' % (a_owner, a_name), '%s::%s' % (b_owner, b_name)))
        elif a_owner != b_owner:
            owner_diff.append((s, '%s::%s' % (a_owner, a_name), '%s::%s' % (b_owner, b_name)))
    extra = sorted(s for s in got if s > last)
    return dict(ok=not bad and not extra, bad=bad, owner_diff=owner_diff, compared=compared, extra=extra, unnamed=unnamed,
                clang_slots=max(got) if got else 0, apple_slots=last)


def check_main(d, sdk, want):
    classes = ORDER if want == ['all'] else want
    tmp = tempfile.mkdtemp()
    texts = {}
    for c in ORDER:
        texts[c], _ = render(d, c, sdk)
        open(os.path.join(tmp, c + '.h'), 'w').write(texts[c])
    rc = 0
    for c in classes:
        r = check_class(d, sdk, c, tmp, texts)
        if r.get('msg'):
            print('%-28s CANNOT BE VERIFIED: %s' % (c, r['msg']))
            rc = 1
            continue
        print('%-28s slots apple=%d clang=%d compared=%d mismatches=%d owner-diffs=%d unnamed-pure=%s' % (
            c, r['apple_slots'], r['clang_slots'], r['compared'], len(r['bad']) + len(r['extra']), len(r['owner_diff']),
            r['unnamed'] or '-'))
        for s, a, b in r['bad'][:25]:
            print('    slot %d: apple %-60s clang %s' % (s, a, b))
        for s in r['extra'][:10]:
            print('    slot %d: present in clang layout, not in Apple vtable' % s)
        for s, a, b in r['owner_diff'][:6]:
            print('    note slot %d: apple %s, header resolves to %s (override not redeclared)' % (s, a, b))
        if not r['ok']:
            rc = 1
    return rc
