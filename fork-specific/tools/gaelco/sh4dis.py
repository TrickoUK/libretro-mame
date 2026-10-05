#!/usr/bin/env python3
# usage: sh4dis.py <ramdump> <base_hex> <start_hex> <len_hex>
# SH4 disassembly via capstone, with PC-relative literal loads resolved to their values.
import sys, re, struct, capstone
data = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16); start = int(sys.argv[3], 16); n = int(sys.argv[4], 16)
md = capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_SH4 | capstone.CS_MODE_SHFPU | capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True
def off(a): return (a & 0x1fffffff) - (base & 0x1fffffff)
for i in md.disasm(data[off(start):off(start)+n], start):
    extra = ""
    m = re.match(r"(mov\.[lw])\s+(0x[0-9a-f]+),", i.mnemonic + " " + i.op_str)
    if m:
        a = int(m.group(2), 16); o = off(a)
        if 0 <= o < len(data) - 4:
            v = struct.unpack_from("<I" if m.group(1) == "mov.l" else "<h", data, o)[0]
            extra = "   ; = %#x" % (v & 0xffffffff)
    print("%08x: %-8s %s%s" % (i.address, i.mnemonic, i.op_str, extra))
