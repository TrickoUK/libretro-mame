#!/usr/bin/env python3
# usage: texvariants.py <gpuram.bin> <list_start> <tex_low_hex> <out.png>: decode one texture several ways
import struct, sys
from PIL import Image
def load(fn):
    d=open(fn,'rb').read(); pages={}; i=0
    while i<len(d):
        p=struct.unpack_from('<I',d,i)[0]; pages[p]=d[i+4:i+4+4096]; i+=4+4096
    return pages
b=load(sys.argv[1]); start=int(sys.argv[2],16); want=int(sys.argv[3],16)
def rb(a):
    pg=b.get((a-0x18000000)>>12); return pg[a&0xfff] if pg else 0
def r16(a): return rb(a)|(rb(a+1)<<8)
def rd(a):
    pg=b.get((a-0x18000000)>>12); return struct.unpack_from('<I',pg,a&0xfff)[0] if pg else 0
DIL=[sum(((v>>i)&1)<<(2*i) for i in range(10)) for v in range(1024)]
a=start; found=None
while True:
    w=rd(a)
    if w==0 or w==0xdeadbeef: break
    if w>>31:
        tsp,tex=rd(a+16),rd(a+20)
        if (tex&0x1fffff)==want: found=(tex,tsp); break
        a+=24
    else: a+=4
tex,tsp=found; print(hex(tex),hex(tsp))
mip=(tex>>31)&1; pf=(tex>>27)&7
base=0x19000000+((tex&0x1fffff)<<3)
su=(tsp>>3)&7; sv=tsp&7; w=8<<su; h=8<<sv
print('size',w,h,'mip',mip)
mipvq=[0x6,0x16,0x56,0x156,0x556,0x1556,0x5556,0x15556]
def conv(v): return (((v>>11)&31)<<3,((v>>5)&63)<<2,(v&31)<<3)
variants=[]
def mk(name,idxaddr,idxfn,cbfn):
    im=Image.new('RGB',(w,h)); px=im.load()
    for y in range(h):
        for x in range(w):
            idx=rb(idxaddr+idxfn(x,y)); px[x,y]=conv(r16(base+idx*8+cbfn(x,y)))
    variants.append((name,im))
tw=lambda x,y:(DIL[x>>1]<<1)+DIL[y>>1]
lin=lambda x,y:(y>>1)*(w>>1)+(x>>1)
cbt=lambda x,y:((DIL[x&1]<<1)+DIL[y&1])*2
cbl=lambda x,y:((y&1)*2+(x&1))*2
i0=base+0x800+(mipvq[su] if mip else 0)
mk('tw/tw mip',i0,tw,cbt)
mk('lin/tw mip',i0,lin,cbt)
mk('tw/lin mip',i0,tw,cbl)
mk('tw/tw nomip',base+0x800,tw,cbt)
# swapped xy
tw2=lambda x,y:(DIL[y>>1]<<1)+DIL[x>>1]
mk('tw swapped',i0,tw2,cbt)
sheet=Image.new('RGB',(w*len(variants)+10*len(variants),h),(40,0,0))
for i,(n,im) in enumerate(variants):
    sheet.paste(im,(i*(w+10),0)); print(i,n)
sheet.save(sys.argv[4])
