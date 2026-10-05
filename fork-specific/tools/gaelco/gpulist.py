#!/usr/bin/env python3
# Parse a sparse GPU RAM dump (dump_gpuram.lua) and plot the object list as a wireframe.
# usage: gpulist.py <gpuram.bin> <list_start_hex> <out.png> [max_words_hex]
import struct, sys, collections
from PIL import Image, ImageDraw
def load(fn):
    d=open(fn,'rb').read(); pages={}; i=0
    while i<len(d):
        p=struct.unpack_from('<I',d,i)[0]; pages[p]=d[i+4:i+4+4096]; i+=4+4096
    return pages
b=load(sys.argv[1]); start=int(sys.argv[2],16); out=sys.argv[3]
maxw=int(sys.argv[4],16) if len(sys.argv)>4 else 0x2000
def rd(a):
    pg=b.get((a-0x18000000)>>12); return struct.unpack_from('<I',pg,a&0xfff)[0] if pg else 0
def rdf(a): return struct.unpack('<f',struct.pack('<I',rd(a)))[0]
im=Image.new('RGB',(640,480),(20,20,30)); dr=ImageDraw.Draw(im)
a=start; state=None; ntri=0; nstate=0; nent=0; unk=collections.Counter()
end=start+maxw*4
while a<end:
    w=rd(a)
    if w==0 or w==0xdeadbeef: break
    if (w>>31)==1:
        state=(rd(a+4),rd(a+8),rd(a+12),rd(a+16),rd(a+20)); nstate+=1
        ent=w; a+=24
    elif state:
        ent=w; a+=4
    else:
        unk[hex(w>>28)]+=1; a+=4; continue
    nent+=1
    mask=(ent>>25)&0x3f; skip=(ent>>21)&7; off=ent&0x1fffff
    vsz=(skip+3)*4
    base=state[0]
    col=(60+(state[4]*37)%190,60+(state[4]*91)%190,60+(state[4]*53)%190)
    for k in range(6):
        if mask&(0x20>>k):
            vs=[base+off*4+vsz*(k+j) for j in range(3)]
            pts=[(rdf(v),rdf(v+4)) for v in vs]
            dr.polygon(pts,outline=col); ntri+=1
print('entries',nent,'states',nstate,'triangles',ntri,'unknown words',dict(unk),'ended at %08x'%a)
im.save(out)
