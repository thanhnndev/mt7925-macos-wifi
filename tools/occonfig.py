#!/usr/bin/env python3
"""Typed get/set/check for an OpenCore config.plist, so edits to the ESP stop being hand-typed plist surgery.

  occonfig.py [-c CONFIG] get   PATH [--reveal]
  occonfig.py [-c CONFIG] set   PATH VALUE [--tag NAME] [--dry] [--force]
  occonfig.py [-c CONFIG] add   PATH JSON  [--tag NAME] [--dry] [--force]   append one element to an array
  occonfig.py [-c CONFIG] del   PATH       [--tag NAME] [--dry]
  occonfig.py [-c CONFIG] args  show | add|del [--tag NAME] [--dry] -- ARG...    NVRAM boot-args (args start with '-',
                                                                               hence the --; foo=1 replaces foo=0)
  occonfig.py [-c CONFIG] check [--ocvalidate BIN]
  occonfig.py [-c CONFIG] diff  OTHER.plist [--reveal]      '-' only in CONFIG, '+' only in OTHER, '~' differs
  occonfig.py schema SAMPLE.plist > tools/occonfig-schema.json

PATH is '/'-separated: dict keys, array indexes, or [Key=Value] to pick an array element by one of its fields:
  Kernel/Add/[BundlePath=Lilu.kext]/Enabled      UEFI/Quirks/ResizeGpuBars      Booter/Quirks/ProvideCustomSlide
  NVRAM/Add/7C436110-AB2A-4BBB-A880-FE41995C9F82/boot-args

Types come from the schema (tools/occonfig-schema.json, generated from OpenCorePkg Docs/Sample.plist of the pinned
commit), so `set UEFI/Quirks/ResizeGpuBars 13` writes an <integer>, `set Misc/Security/Vault Optional` a <string>,
`set NVRAM/Add/<GUID>/csr-active-config 430a0000` <data>. A key the schema does not know is refused unless --force.
Every write first copies the file to config.plist.<tag> (default occonfig-<timestamp>) in the same directory.
PlatformInfo identifiers (serial, MLB, UUID, ROM) are masked in output unless --reveal.

CONFIG defaults to $OC_CONFIG, else /boot/EFI/OC/config.plist when it exists, else efi/OC/config.plist.
`check` also runs ocvalidate when found ($OCVALIDATE, --ocvalidate, ~/.cache/ga403/bin/ocvalidate, or PATH);
scripts/build-ocvalidate.sh builds it from the OpenCorePkg commit the ESP's OpenCore was built from.
"""
import argparse
import datetime
import json
import os
import plistlib
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA_FILE = os.path.join(HERE, "occonfig-schema.json")
SECRET_KEYS = {"SystemSerialNumber", "MLB", "SystemUUID", "ROM", "CustomUUID"}
OPEN_MAPS = {"NVRAM/Add", "NVRAM/Delete", "NVRAM/LegacySchema", "DeviceProperties/Add", "DeviceProperties/Delete"}
TYPES = {bool: "bool", int: "integer", float: "real", str: "string", bytes: "data", list: "array", dict: "dict"}


class Err(SystemExit):
    def __init__(self, msg):
        super().__init__("occonfig: " + msg)


# ----------------------------------------------------------------------------------------------- schema
def build_schema(v, path=""):
    """Sample.plist value -> schema node: {"t": type, "keys": {...}} / {"t": "array", "item": node|None} / {"t": scalar}."""
    t = TYPES[type(v)]
    if t == "dict":
        if path in OPEN_MAPS:
            return {"t": "dict", "open": True}
        return {"t": "dict", "keys": {k: build_schema(x, path + "/" + k if path else k)
                                      for k, x in v.items() if not k.startswith("#")}}   # "#WARNING - n" notes
    if t == "array":
        return {"t": "array", "item": build_schema(v[0], path + "/*") if v else None}
    return {"t": t}


def load_schema():
    try:
        with open(SCHEMA_FILE) as f:
            return json.load(f)
    except OSError:
        raise Err("%s is missing; generate it with: occonfig.py schema Docs/Sample.plist > %s" % (SCHEMA_FILE, SCHEMA_FILE))


# ----------------------------------------------------------------------------------------------- plist io
def default_config():
    if os.environ.get("OC_CONFIG"):
        return os.environ["OC_CONFIG"]
    if os.path.exists("/boot/EFI/OC/config.plist"):
        return "/boot/EFI/OC/config.plist"
    return os.path.join(HERE, "..", "efi", "OC", "config.plist")


def load(path):
    try:
        with open(path, "rb") as f:
            return plistlib.load(f)
    except (OSError, plistlib.InvalidFileException, ValueError) as e:
        raise Err("cannot read %s: %s" % (path, e))


def dumps(cfg):
    """XML plist, keys in file order (no sorting) so edits diff cleanly."""
    return plistlib.dumps(cfg, fmt=plistlib.FMT_XML, sort_keys=False)


def save(path, cfg, tag, dry):
    if dry:
        print("(dry run, nothing written)")
        return
    bak = "%s.%s" % (path, tag or "occonfig-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    if os.path.exists(bak):
        raise Err("backup %s already exists; pick another --tag" % bak)
    shutil.copy2(path, bak)
    tmp = path + ".occonfig-tmp"
    with open(tmp, "wb") as f:
        f.write(dumps(cfg))
    with open(tmp, "rb") as f:                      # what we wrote must parse again
        plistlib.load(f)
    os.replace(tmp, path)
    print("wrote %s (backup: %s)" % (path, os.path.basename(bak)))


# ----------------------------------------------------------------------------------------------- paths
def split(path):
    return [p for p in path.split("/") if p != ""]


def step(node, sch, tok, create=False):
    """Descend one token. Returns (container, key, child_schema); container[key] is the child (may not exist yet)."""
    if isinstance(node, list):
        if tok.startswith("[") and tok.endswith("]") and "=" in tok:
            k, _, want = tok[1:-1].partition("=")
            for i, el in enumerate(node):
                if isinstance(el, dict) and str(el.get(k)) == want:
                    return node, i, (sch or {}).get("item")
            raise Err("no array element with %s=%s" % (k, want))
        try:
            i = int(tok)
        except ValueError:
            raise Err("array index or [Key=Value] expected, got %r" % tok)
        if not -len(node) <= i < len(node):
            raise Err("index %d out of range (array has %d)" % (i, len(node)))
        return node, i, (sch or {}).get("item")
    if isinstance(node, dict):
        child = None
        if sch and sch.get("keys") is not None:
            child = sch["keys"].get(tok)
        return node, tok, child
    raise Err("cannot descend into a %s with %r" % (TYPES.get(type(node), type(node).__name__), tok))


def resolve(cfg, schema, path, strict_schema):
    toks = split(path)
    if not toks:
        raise Err("empty path")
    node, sch = cfg, schema
    for n, tok in enumerate(toks):
        cont, key, csch = step(node, sch, tok)
        last = n == len(toks) - 1
        if isinstance(cont, dict) and key not in cont:
            if not last:
                raise Err("no such key: %s" % "/".join(toks[: n + 1]))
            if csch is None and strict_schema and sch and sch.get("keys") is not None:
                near = [k for k in sch["keys"] if k.lower().startswith(tok.lower()[:3])]
                raise Err("%r is not an OpenCore key under %s%s (use --force to add it anyway)"
                          % (tok, "/".join(toks[:n]) or "/", "; similar: " + ", ".join(near) if near else ""))
            return cont, key, csch
        if last:
            return cont, key, csch
        node, sch = cont[key], csch
    raise Err("unreachable")


# ----------------------------------------------------------------------------------------------- values
def coerce(text, want, current=None):
    """VALUE text -> python value of schema type `want` (or inferred from `current`/the text when unknown)."""
    if want is None:
        want = TYPES.get(type(current)) if current is not None else None
    if want is None:
        low = text.lower()
        if low in ("true", "false"):
            want = "bool"
        elif text.lstrip("-").isdigit() or text.lower().startswith("0x"):
            want = "integer"
        elif text.startswith("hex:"):
            want, text = "data", text[4:]
        else:
            want = "string"
    try:
        if want == "bool":
            low = text.lower()
            if low in ("true", "1", "yes", "on"):
                return True
            if low in ("false", "0", "no", "off"):
                return False
            raise ValueError("expected true/false")
        if want == "integer":
            return int(text, 0)
        if want == "real":
            return float(text)
        if want == "data":
            h = text.replace(" ", "").replace(":", "")
            return bytes.fromhex(h[2:] if h.lower().startswith("0x") else h)
        if want == "string":
            return text
    except ValueError as e:
        raise Err("%r is not a valid %s (%s)" % (text, want, e))
    raise Err("cannot set a %s from the command line; use `add` with JSON for array elements" % want)


def from_json(j, sch):
    """JSON (dict/list/scalars, data as 'hex:..') -> plist value, using the schema for the element's types when given."""
    if isinstance(j, dict):
        keys = (sch or {}).get("keys") or {}
        return {k: from_json(v, keys.get(k)) for k, v in j.items()}
    if isinstance(j, list):
        return [from_json(v, (sch or {}).get("item")) for v in j]
    if sch and sch.get("t") == "data" and isinstance(j, str):
        return coerce(j, "data")
    if sch and sch.get("t") == "integer" and isinstance(j, str):
        return coerce(j, "integer")
    if isinstance(j, str) and j.startswith("hex:"):
        return coerce(j[4:], "data")
    return j


def show(v, reveal, key=None):
    if key in SECRET_KEYS and not reveal:
        return "<masked>"
    if isinstance(v, dict):
        return {k: show(x, reveal, k) for k, x in v.items()}
    if isinstance(v, list):
        return [show(x, reveal) for x in v]
    if isinstance(v, bytes):
        return "hex:" + v.hex()
    if isinstance(v, datetime.datetime):
        return v.isoformat()
    return v


# ----------------------------------------------------------------------------------------------- check
def typecheck(v, sch, path, out):
    if sch is None:
        return
    t = TYPES.get(type(v), type(v).__name__)
    want = sch["t"]
    if t != want and not (want == "real" and t == "integer"):
        out.append("%s: is <%s>, OpenCore expects <%s>" % (path or "/", t, want))
        return
    if want == "dict":
        keys = sch.get("keys")
        if keys is None:
            return
        for k in v:
            if k not in keys:
                out.append("%s/%s: unknown key (not in OpenCore's schema)" % (path, k))
        for k, s in keys.items():
            if k not in v:
                out.append("%s/%s: missing key" % (path, k))
            else:
                typecheck(v[k], s, path + "/" + k, out)
    elif want == "array" and sch.get("item"):
        for i, el in enumerate(v):
            typecheck(el, sch["item"], "%s[%d]" % (path, i), out)


def find_ocvalidate(opt):
    for c in (opt, os.environ.get("OCVALIDATE"), os.path.expanduser("~/.cache/ga403/bin/ocvalidate"),
              shutil.which("ocvalidate")):
        if c and os.access(c, os.X_OK):
            return c
    return None


def cmd_check(a):
    cfg, schema = load(a.config), load_schema()
    out = []
    typecheck(cfg, schema, "", out)
    for line in out:
        print("TYPE", line)
    print("schema: %d issue(s)" % len(out))
    oc = find_ocvalidate(a.ocvalidate)
    rc = 0 if not out else 1
    if oc:
        p = subprocess.run([oc, a.config], capture_output=True, text=True)
        text = (p.stdout + p.stderr).strip()
        print("ocvalidate (%s):\n%s" % (oc, text))
        # ocvalidate exits non-zero for errors; its "NOTE" line only states the OpenCore version it matches.
        if p.returncode != 0:
            rc = 1
    else:
        print("ocvalidate: not found (scripts/build-ocvalidate.sh builds it); schema check only")
    return rc


# ----------------------------------------------------------------------------------------------- commands
def cmd_get(a):
    cfg = load(a.config)
    toks = split(a.path)
    node = cfg
    for tok in toks:
        cont, key, _ = step(node, None, tok)
        if isinstance(cont, dict) and key not in cont:
            raise Err("no such key: %s" % a.path)
        node = cont[key]
    last = toks[-1] if toks else None
    v = show(node, a.reveal, last)
    print(json.dumps(v, indent=2, ensure_ascii=False) if isinstance(v, (dict, list)) else
          (str(v).lower() if isinstance(v, bool) else v))
    return 0


def cmd_set(a):
    cfg, schema = load(a.config), load_schema()
    cont, key, sch = resolve(cfg, schema, a.path, not a.force)
    exists = key in cont if isinstance(cont, dict) else True
    cur = cont[key] if exists else None
    new = coerce(a.value, sch["t"] if sch else None, cur)
    name = split(a.path)[-1]
    if exists and cur == new and type(cur) is type(new):
        print("%s already %r; nothing to do" % (a.path, show(new, False, name)))
        return 0
    print("%s: %r -> %r" % (a.path, show(cur, False, name), show(new, False, name)))
    cont[key] = new
    save(a.config, cfg, a.tag, a.dry)
    return 0


def cmd_add(a):
    cfg, schema = load(a.config), load_schema()
    cont, key, sch = resolve(cfg, schema, a.path, not a.force)
    arr = cont[key]
    if not isinstance(arr, list):
        raise Err("%s is not an array" % a.path)
    try:
        el = from_json(json.loads(a.json), (sch or {}).get("item"))
    except json.JSONDecodeError as e:
        raise Err("JSON: %s" % e)
    out = []
    typecheck(el, (sch or {}).get("item"), a.path + "[+]", out)
    if out and not a.force:
        raise Err("element does not match the schema:\n  " + "\n  ".join(out))
    arr.append(el)
    print("%s: appended element %d" % (a.path, len(arr) - 1))
    save(a.config, cfg, a.tag, a.dry)
    return 0


def cmd_del(a):
    cfg, schema = load(a.config), load_schema()
    cont, key, _ = resolve(cfg, schema, a.path, False)
    if isinstance(cont, dict) and key not in cont:
        raise Err("no such key: %s" % a.path)
    del cont[key]
    print("%s: removed" % a.path)
    save(a.config, cfg, a.tag, a.dry)
    return 0


BOOT_ARGS = "NVRAM/Add/7C436110-AB2A-4BBB-A880-FE41995C9F82/boot-args"


def cmd_args(a):
    cfg = load(a.config)
    try:
        cur = cfg["NVRAM"]["Add"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]["boot-args"]
    except KeyError:
        cur = ""
    words = cur.split()
    if a.action == "show":
        print("\n".join(words))
        return 0
    if not a.items:
        raise Err("args %s needs at least one argument" % a.action)
    new = list(words)
    for it in a.items:
        name = it.split("=")[0]
        new = [w for w in new if w.split("=")[0] != name]      # an arg is replaced by name, `foo=1` -> `foo=2`
        if a.action == "add":
            new.append(it)
    if new == words:
        print("boot-args unchanged")
        return 0
    print("boot-args: %s" % " ".join(new))
    cfg["NVRAM"]["Add"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]["boot-args"] = " ".join(new)
    save(a.config, cfg, a.tag, a.dry)
    return 0


def cmd_diff(a):
    x, y = load(a.config), load(a.other)
    n = [0]

    def emit(kind, p, *vals):
        n[0] += 1
        print(kind, p, *vals)

    def short(v, key=None):
        s = repr(show(v, a.reveal, key))
        return s if len(s) < 140 else s[:137] + "..."

    def walk(u, v, p, key=None):
        if isinstance(u, dict) and isinstance(v, dict):
            for k in u.keys() | v.keys():
                if k not in u:
                    emit("+", p + "/" + k, short(v[k], k))
                elif k not in v:
                    emit("-", p + "/" + k, short(u[k], k))
                else:
                    walk(u[k], v[k], p + "/" + k, k)
        elif isinstance(u, list) and isinstance(v, list):
            if len(u) == len(v):
                for i, (s, t) in enumerate(zip(u, v)):
                    walk(s, t, "%s[%d]" % (p, i))
            else:
                emit("~", p, "%d element(s) vs %d" % (len(u), len(v)))
                for i, el in enumerate(v):
                    if el not in u:
                        emit("+", "%s[%d]" % (p, i), short(el))
                for i, el in enumerate(u):
                    if el not in v:
                        emit("-", "%s[%d]" % (p, i), short(el))
        elif u != v:
            emit("~", p, short(u, key), "->", short(v, key))

    walk(x, y, "")
    print("%d difference(s) (%s -> %s)" % (n[0], a.config, a.other))
    return 0


def cmd_schema(a):
    json.dump(build_schema(load(a.sample)), sys.stdout, indent=1, sort_keys=False)
    sys.stdout.write("\n")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-c", "--config", default=None)
    sub = p.add_subparsers(dest="cmd", required=True)

    def writer(s):
        s.add_argument("--tag", help="backup suffix (default occonfig-<timestamp>)")
        s.add_argument("--dry", action="store_true", help="show the change, write nothing")
        s.add_argument("--force", action="store_true", help="allow keys/elements the schema does not know")
        return s

    s = sub.add_parser("get"); s.add_argument("path"); s.add_argument("--reveal", action="store_true"); s.set_defaults(f=cmd_get)
    s = writer(sub.add_parser("set")); s.add_argument("path"); s.add_argument("value"); s.set_defaults(f=cmd_set)
    s = writer(sub.add_parser("add")); s.add_argument("path"); s.add_argument("json"); s.set_defaults(f=cmd_add)
    s = writer(sub.add_parser("del")); s.add_argument("path"); s.set_defaults(f=cmd_del)
    s = writer(sub.add_parser("args")); s.add_argument("action", choices=("show", "add", "del"))
    s.add_argument("items", nargs="*"); s.set_defaults(f=cmd_args)
    s = sub.add_parser("check"); s.add_argument("--ocvalidate"); s.set_defaults(f=cmd_check)
    s = sub.add_parser("diff"); s.add_argument("other"); s.add_argument("--reveal", action="store_true"); s.set_defaults(f=cmd_diff)
    s = sub.add_parser("schema"); s.add_argument("sample"); s.set_defaults(f=cmd_schema)
    a = p.parse_args(argv)
    a.config = a.config or default_config()
    return a.f(a)


if __name__ == "__main__":
    sys.exit(main())
