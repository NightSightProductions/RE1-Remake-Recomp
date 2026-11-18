#!/usr/bin/env python3
"""Removes the SteamStub v3.1 (x86) wrapper from the user's own copy of bhd.exe, so the translator can read
the game's code. Run at build time on the user's machine; the output is never distributed.

Same scheme as v3.0 x64 (see Wolfenstein-TNO-Recomp/tools/unwrap_steamstub.py), different header:
  - the 0xF0-byte header sits just before the stub entry point in .bind, XOR-chained with its first dword,
    signature 0xC0DEC0DF;
  - .text is AES-256-CBC encrypted, its first 16 bytes moved into the header ("stolen" bytes), and the IV
    itself ECB-encrypted with the same key.
Header fields (offsets after decoding): 0x08 ImageBase (u64), 0x10 stub entry (u64), 0x20 OEP (u64, RVA),
0x38 app id, 0x3C flags, 0x48 code RVA (u64), 0x50 code raw size (u64), 0x58 AES key, 0x78 IV, 0x88 stolen.

Usage: unwrap_steamstub32.py bhd.exe out.exe
"""
import struct, subprocess, sys

HDR_SIZE = 0xF0
SIGNATURE = 0xC0DEC0DF
FLAG_NO_ENCRYPTION = 0x04


def die(msg):
    sys.exit(f"unwrap_steamstub32: {msg}")


def sections(pe):
    e_lfanew = struct.unpack_from('<I', pe, 0x3C)[0]
    nsec = struct.unpack_from('<H', pe, e_lfanew + 6)[0]
    opt_size = struct.unpack_from('<H', pe, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    if struct.unpack_from('<H', pe, opt)[0] != 0x10B:
        die("not a PE32 (x86) image")
    tab = opt + opt_size
    out = []
    for i in range(nsec):
        s = tab + i * 40
        name = pe[s:s + 8].rstrip(b'\0').decode('latin-1')
        vsize, va, rawsize, rawoff = struct.unpack_from('<IIII', pe, s + 8)
        out.append((name, va, vsize, rawoff, rawsize))
    return opt, out


def rva_to_off(secs, rva):
    for _, va, vsize, rawoff, rawsize in secs:
        if va <= rva < va + max(vsize, rawsize):
            return rva - va + rawoff
    die(f"RVA {rva:#x} is outside every section")


def aes(mode, key, data, iv=None):
    cmd = ['openssl', 'enc', '-d', f'-aes-256-{mode}', '-nopad', '-K', key.hex()]
    if iv is not None:
        cmd += ['-iv', iv.hex()]
    r = subprocess.run(cmd, input=data, capture_output=True)
    if r.returncode != 0 or len(r.stdout) != len(data):
        die(f"openssl failed: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout


def main():
    if len(sys.argv) != 3:
        die("usage: unwrap_steamstub32.py in.exe out.exe")
    pe = bytearray(open(sys.argv[1], 'rb').read())
    opt, secs = sections(pe)
    ep = struct.unpack_from('<I', pe, opt + 16)[0]
    ep_off = rva_to_off(secs, ep)

    h = bytearray(pe[ep_off - HDR_SIZE:ep_off])
    key = struct.unpack_from('<I', h, 0)[0]
    for x in range(4, HDR_SIZE, 4):
        v = struct.unpack_from('<I', h, x)[0]
        struct.pack_into('<I', h, x, v ^ key)
        key = v
    if struct.unpack_from('<I', h, 4)[0] != SIGNATURE:
        die("no SteamStub v3.1 x86 header at the entry point (already unwrapped, or another variant)")

    stub_ep, = struct.unpack_from('<Q', h, 0x10)
    oep, = struct.unpack_from('<Q', h, 0x20)
    app_id, flags = struct.unpack_from('<II', h, 0x38)
    code_va, code_raw = struct.unpack_from('<QQ', h, 0x48)
    aes_key, aes_iv, stolen = bytes(h[0x58:0x78]), bytes(h[0x78:0x88]), bytes(h[0x88:0x98])
    if stub_ep != ep:
        die(f"header entry {stub_ep:#x} != PE entry {ep:#x}")
    print(f"SteamStub v3.1 x86: app {app_id}, flags {flags:#x}, OEP {oep:#x}, code {code_va:#x}+{code_raw:#x}")

    code = [s for s in secs if s[1] == code_va]
    if not code:
        die(f"no section at code RVA {code_va:#x}")
    _, _, _, rawoff, rawsize = code[0]
    if rawsize != code_raw or rawsize % 16:
        die(f"unexpected code section size {rawsize:#x} (header says {code_raw:#x})")

    if not flags & FLAG_NO_ENCRYPTION:
        iv = aes('ecb', aes_key, aes_iv)
        plain = aes('cbc', aes_key, stolen + bytes(pe[rawoff:rawoff + rawsize]), iv)
        pe[rawoff:rawoff + rawsize] = plain[:rawsize]

    struct.pack_into('<I', pe, opt + 16, oep)          # AddressOfEntryPoint
    struct.pack_into('<I', pe, opt + 64, 0)            # CheckSum: no longer valid
    open(sys.argv[2], 'wb').write(pe)
    print(f"wrote {sys.argv[2]}")


if __name__ == '__main__':
    main()
