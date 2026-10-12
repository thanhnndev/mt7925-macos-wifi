# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Small helper to explore BootKernelExtensions.kc (x86_64 Mach-O fileset): symbols, string xrefs, disassembly."""
import os, struct, re, sys, bisect
try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
except ImportError:      # symbols/vtables work without it; only disasm() needs Capstone
    Cs = None

KC = os.environ.get('KC') or (sys.argv[1] if len(sys.argv) > 1 else None)
if not KC or not os.path.isfile(KC):
    sys.exit('set KC=<path to BootKernelExtensions.kc> (see docs/USB-XHCI-LAG.md)')
d = open(KC, 'rb').read()

segs = []          # (name, vmaddr, vmsize, fileoff, filesize, prot)
symtabs = []       # (symoff, nsyms, stroff, strsize)
entries = []


def parse_header(off, top=True):
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IIIIIIII', d, off)
    p = off + 32
    out = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, p)
        out.append((cmd, p, size))
        p += size
    return out


def load():
    for cmd, p, size in parse_header(0):
        if cmd == 0x19:
            name = d[p + 8:p + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', d, p + 24)
            maxprot, initprot = struct.unpack_from('<II', d, p + 56)
            segs.append((name, vmaddr, vmsize, fileoff, filesize, initprot))
        elif cmd == 0x2:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', d, p + 8)
            symtabs.append((symoff, nsyms, stroff, strsize))
        elif cmd == 0x80000035:
            vmaddr, fileoff, strx, _ = struct.unpack_from('<QQII', d, p + 8)
            ename = d[p + strx:d.index(b'\0', p + strx)].decode()
            entries.append((ename, vmaddr, fileoff))
            for c2, p2, s2 in parse_header(fileoff):
                if c2 == 0x2:
                    so, ns, sto, sts = struct.unpack_from('<IIII', d, p2 + 8)
                    if ns:
                        symtabs.append((so, ns, sto, sts))


load()


def va2off(va):
    for name, vm, sz, fo, fs, pr in segs:
        if vm <= va < vm + min(sz, fs if fs else sz):
            return fo + (va - vm)
    return None


def off2va(off):
    for name, vm, sz, fo, fs, pr in segs:
        if fo <= off < fo + fs:
            return vm + (off - fo)
    return None


syms = {}
names = {}
for symoff, nsyms, stroff, strsize in symtabs:
    for i in range(nsyms):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', d, symoff + i * 16)
        if n_value and n_strx < strsize and (n_type & 0x0e) == 0x0e:
            end = d.index(b'\0', stroff + n_strx)
            nm = d[stroff + n_strx:end].decode('latin1')
            syms[n_value] = nm
sorted_addrs = sorted(syms)


def symname(va):
    i = bisect.bisect_right(sorted_addrs, va) - 1
    if i < 0:
        return None
    a = sorted_addrs[i]
    return '%s+%#x' % (syms[a], va - a) if va != a else syms[a]


def demangle(s):
    return s


md = Cs(CS_ARCH_X86, CS_MODE_64) if Cs else None
if md:
    md.detail = False


def find_func_start(va):
    """nearest known symbol start at or below va"""
    i = bisect.bisect_right(sorted_addrs, va) - 1
    return sorted_addrs[i] if i >= 0 else None


def disasm(start, end, comment_strings=True):
    if md is None:
        raise RuntimeError('disasm needs Capstone (pip install capstone); llvm-objdump works on the extracted kext too')
    off = va2off(start)
    code = d[off:off + (end - start)]
    for ins in md.disasm(code, start):
        txt = '%x: %-8s %s' % (ins.address, ins.mnemonic, ins.op_str)
        # annotate rip-relative and call targets
        m = re.search(r'rip \+ (0x[0-9a-f]+|\d+)\]', ins.op_str)
        note = ''
        if m:
            tgt = ins.address + ins.size + int(m.group(1), 0)
            o = va2off(tgt)
            if o is not None:
                s = d[o:o + 80].split(b'\0')[0]
                if len(s) >= 4 and all(0x20 <= c < 0x7f for c in s):
                    note = '  ; "%s"' % s.decode()
                else:
                    note = '  ; %#x' % tgt
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            nm = symname(int(ins.op_str, 16))
            if nm:
                note = '  ; ' + nm
        print(txt + note)


def xrefs_to(va_target):
    res = []
    for name, vm, sz, fo, fs, pr in segs:
        if not (pr & 4) and 'TEXT' not in name:
            continue
        seg = d[fo:fo + fs]
        for m in re.finditer(rb'[\x48\x4c\x8d]', seg):
            pass
    return res


def find_string_va(s):
    b = s.encode() if isinstance(s, str) else s
    res = []
    i = d.find(b)
    while i >= 0:
        va = off2va(i)
        if va is not None:
            res.append(va)
        i = d.find(b, i + 1)
    return res


_lea = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]|[\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')


def xrefs(targets):
    tset = set(targets)
    out = {}
    for name, vm, sz, fo, fs, pr in segs:
        if not (pr & 4):
            continue
        seg = d[fo:fo + fs]
        for m in _lea.finditer(seg):
            p = m.start()
            if p + 7 > len(seg):
                continue
            disp = struct.unpack_from('<i', seg, p + 3)[0]
            tgt = vm + p + 7 + disp
            if tgt in tset:
                out.setdefault(tgt, []).append(vm + p)
    return out


def func_range(va):
    i = bisect.bisect_right(sorted_addrs, va) - 1
    a = sorted_addrs[i]
    b = sorted_addrs[i + 1] if i + 1 < len(sorted_addrs) else a + 0x400
    return a, b
