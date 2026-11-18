# usage: d3dcalls.py ASMFILE START END: D3D9 device calls (vtable offsets named from the bridge table) and direct calls
# in an address range of a disasm.py listing
import re, sys
names = {}
src = open("framework/win32/runtime/d3d9_bridge.cpp").read()
dev = src[src.find("const ComMethod device_m[]"):]
dev = dev[:dev.find("};")]
for k, m in enumerate(re.findall(r'\b(?:FWD|SPC)\("(\w+)"', dev)): names[(3 + k) * 4] = m   # after the 3 IUnknown slots
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
out = []
for ln in open(sys.argv[1]):
    a = int(ln.split(":")[0], 16)
    if not lo <= a < hi: continue
    m = re.search(r"call dword ptr \[e.. \+ (0x[0-9a-f]+)\]", ln)
    if m: out.append(names.get(int(m.group(1), 16), "vt+" + m.group(1)))
    elif re.search(r"call 0x", ln): out.append("F_" + ln.split("call 0x")[1].strip())
print(" ".join(out))
