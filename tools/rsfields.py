# usage: rsfields.py ADDR: list the (word offset, shift, mask) -> D3DRS / sampler state pairs an engine state decoder
# feeds to SetRenderState / SetSamplerState (decoders compare old/new fields and set the changed ones)
import re, subprocess, sys
RS = {0x07:"ZENABLE",0x0e:"ZWRITEENABLE",0x0f:"ALPHATESTENABLE",0x13:"SRCBLEND",0x14:"DESTBLEND",0x16:"CULLMODE",0x17:"ZFUNC",
      0x18:"ALPHAREF",0x19:"ALPHAFUNC",0x1b:"ALPHABLENDENABLE",0x34:"STENCILENABLE",0x35:"STENCILFAIL",0x36:"STENCILZFAIL",
      0x37:"STENCILPASS",0x38:"STENCILFUNC",0x39:"STENCILREF",0x3a:"STENCILMASK",0x3b:"STENCILWRITEMASK",0x08:"FILLMODE",
      0x9:"SHADEMODE",0xa8:"COLORWRITEENABLE",0xab:"BLENDOP",0xae:"SCISSORTESTENABLE",0xaf:"SLOPESCALEDEPTHBIAS",0xb0:"ANTIALIASEDLINEENABLE",
      0xb9:"TWOSIDEDSTENCILMODE",0xba:"CCW_STENCILFAIL",0xbb:"CCW_STENCILZFAIL",0xbc:"CCW_STENCILPASS",0xbd:"CCW_STENCILFUNC",
      0xbe:"COLORWRITEENABLE1",0xbf:"COLORWRITEENABLE2",0xc0:"COLORWRITEENABLE3",0xc1:"BLENDFACTOR",0xc2:"SRGBWRITEENABLE",0xc3:"DEPTHBIAS",
      0xce:"SEPARATEALPHABLENDENABLE",0xcf:"SRCBLENDALPHA",0xd0:"DESTBLENDALPHA",0xd1:"BLENDOPALPHA",0xa1:"MULTISAMPLEANTIALIAS",0xa2:"MULTISAMPLEMASK"}
SS = {1:"ADDRESSU",2:"ADDRESSV",3:"ADDRESSW",4:"BORDERCOLOR",5:"MAGFILTER",6:"MINFILTER",7:"MIPFILTER",8:"MIPMAPLODBIAS",9:"MAXMIPLEVEL",
      10:"MAXANISOTROPY",11:"SRGBTEXTURE",12:"ELEMENTINDEX",13:"DMAPOFFSET"}
lines = subprocess.run([sys.executable, "tools/disasm.py", sys.argv[1], "900"], capture_output=True, text=True).stdout.splitlines()
win = []
for ln in lines:
    ins = ln.split(": ", 1)[1]
    m = re.match(r"call dword ptr \[e.. \+ (0x[0-9a-f]+)\]", ins)
    if m:
        off = int(m.group(1), 16)
        pushes = [int(x, 16) for x in re.findall(r"push (0x[0-9a-f]+|\d+)$", "\n".join(w for w in win), re.M)] if False else \
                 [w for w in win if w.startswith("push ")]
        field = [w for w in win if re.match(r"(shr|and|movzx|mov) e.x, (dword|byte|word)? ?(ptr )?\[e.. \+ 0x[0-9a-f]+\]|(shr|and) e.x, 0x", w) or w.startswith(("shr", "and", "movzx"))]
        imms = [p.split()[1] for p in pushes if re.match(r"push (0x[0-9a-f]+|\d+)$", p)]
        if off == 0xe4 and imms:
            st = int(imms[-1], 16) if imms[-1].startswith("0x") else int(imms[-1]); print("RS %-26s <- %s" % (RS.get(st, hex(st)), "; ".join(field[-4:])))
        elif off == 0x114 and imms:
            st = int(imms[-1], 16) if imms[-1].startswith("0x") else int(imms[-1]); print("SS %-26s <- %s" % (SS.get(st, hex(st)), "; ".join(field[-4:])))
        win = []
    else:
        win.append(ins)
    if ins.startswith("ret") and "--all" not in sys.argv: break
