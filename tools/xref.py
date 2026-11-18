# usage: xref.py STRING|0xADDR...: find code in orig.exe referencing a string (exact, NUL-terminated) or an address
import sys, re, struct
sys.path.insert(0, "framework/win32/tools")
import pe32
pe = pe32.PE32("orig.exe")
raw = open("orig.exe", "rb").read()
def va_of(off):
    for _, va, vs, fo, fs, _ in pe.sections:
        if fo <= off < fo + fs: return va + off - fo
    return 0
for arg in sys.argv[1:]:
    targets = [int(arg, 16)] if arg.startswith("0x") else [va_of(m.start()) for m in re.finditer(b"\0" + arg.encode() + b"\0", raw)]
    for t in targets:
        t = t + (0 if arg.startswith("0x") else 1)
        refs = [va_of(m.start()) for m in re.finditer(re.escape(struct.pack("<I", t)), raw)]
        print("%s @%x: refs %s" % (arg, t, " ".join("%x" % r for r in refs[:20])))
