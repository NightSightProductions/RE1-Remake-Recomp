#!/usr/bin/env python3
"""Extract every D3D9 shader (SM2/SM3 token streams) from the game's MT Framework .arc archives.

usage: arc_shaders.py NATIVEPC_DIR OUT_DIR [--types]
  writes OUT_DIR/{vs,ps}_<fnv1a32>.bin (the hash the runtime and tools/sm3msl use); --types only reports which
  entry types hold shaders

.arc (v7): "ARC\\0", u16 version, u16 count, then count entries of 80 bytes: name[64], u32 type hash, u32 compressed
size, u32 size (low 29 bits) | flags, u32 offset; data zlib-compressed.
"""
import os, re, struct, sys, zlib, collections

VER = re.compile(rb"[\x00\x01][\x02\x03][\xfe\xff]\xff")

def fnv(b):
    h = 2166136261
    for x in b: h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h

def shader_at(d, o):
    """the token stream starting at o, up to and including the end token, if it parses; else None"""
    if o % 4 or o + 8 > len(d): return None
    n = o + 4
    while n + 4 <= len(d):
        t = struct.unpack_from("<I", d, n)[0]
        if t == 0x0000FFFF: return d[o:n + 4]
        op = t & 0xFFFF
        ln = (t >> 16) if op == 0xFFFE else (t >> 24) & 0xF
        if op != 0xFFFE and (op > 0x60 and op not in (0x50, 0x51, 0x52, 0x53, 0x5F)): return None
        n += 4 * (1 + ln)
    return None

def entries(path):
    with open(path, "rb") as f: d = f.read()
    if d[:4] != b"ARC\0": return
    ver, cnt = struct.unpack_from("<HH", d, 4)
    for i in range(cnt):
        e = 8 + 80 * i
        name = d[e:e + 64].split(b"\0")[0].decode("latin1")
        typ, csz, dsz, off = struct.unpack_from("<IIII", d, e + 64)
        raw = d[off:off + csz]
        try: data = zlib.decompress(raw) if csz != (dsz & 0x1FFFFFFF) else raw
        except zlib.error: continue
        yield name, typ, data

def main():
    src, out = sys.argv[1], sys.argv[2]
    types_only = "--types" in sys.argv
    os.makedirs(out, exist_ok=True)
    found, by_type, arcs = {}, collections.Counter(), 0
    for root, _, files in os.walk(src):
        for fn in files:
            if not fn.endswith(".arc"): continue
            arcs += 1
            for name, typ, data in entries(os.path.join(root, fn)):
                for m in VER.finditer(data):
                    s = shader_at(data, m.start())
                    if not s: continue
                    by_type[typ] += 1
                    h = fnv(s)
                    kind = "vs" if s[2] == 0xFE else "ps"
                    if h not in found:
                        found[h] = 1
                        if not types_only:
                            with open(os.path.join(out, "%s_%08x.bin" % (kind, h)), "wb") as f: f.write(s)
    print("%d archives, %d unique shaders" % (arcs, len(found)))
    for t, n in by_type.most_common(10): print("  type %08x: %d shader streams" % (t, n))

if __name__ == "__main__":
    main()
