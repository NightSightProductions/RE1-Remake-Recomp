# usage: disasm.py ADDR [COUNT]: disassemble orig.exe at ADDR (stops at int3), import slots annotated
import sys, capstone as cs
sys.path.insert(0, "framework/win32/tools")
import pe32
pe = pe32.PE32("orig.exe")
md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
imp = {}
try:
    for a, n in pe.iat.items(): imp[a] = n
except Exception: pass
a = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 200
data = pe.read(a, 0x4000)
k = 0
for i in md.disasm(data, a):
    s = "%x: %s %s" % (i.address, i.mnemonic, i.op_str)
    for ia, nm in imp.items():
        if "0x%x" % ia in i.op_str: s += "   ; " + str(nm)
    print(s); k += 1
    if k >= n or (i.mnemonic == "int3"): break
