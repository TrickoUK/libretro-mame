#!/usr/bin/env python3
# usage: texsheet.py <gpuram.bin> <list_start_hex> <out.png>  - contact sheet of textures used by the object list
import struct, sys
from PIL import Image, ImageDraw
def load(fn):
    d=open(fn,'rb').read(); pages={}; i=0
    while i<len(d):
        p=struct.unpack_from('<I',d,i)[0]; pages[p]=d[i+4:i+4+4096]; i+=4+4096
    return pages
b=load(sys.argv[1]); start=int(sys.argv[2],16)
def rb(a):
    pg=b.get((a-0x18000000)>>12); return pg[a&0xfff] if pg else 0
def r16(a): return rb(a)|(rb(a+1)<<8)
def rd(a):
    pg=b.get((a-0x18000000)>>12); return struct.unpack_from('<I',pg,a&0xfff)[0] if pg else 0
DIL=[sum(((v>>i)&1)<<(2*i) for i in range(10)) for v in range(1024)]
def decode(tex,tsp):
    mip=(tex>>31)&1; vq=(tex>>30)&1; pf=(tex>>27)&7; scan=(tex>>26)&1
    base=0x18000000+(((tex&0x1fffff)<<5)&0x1ffffff)
    su=1<<(3+((tsp>>3)&7)); sv=1<<(3+(tsp&7)); sz=tsp&7
    if mip: su=sv=1<<(3+(((tsp>>3)&7)))
    mipvq=[0x6,0x16,0x56,0x156,0x556,0x1556,0x5556,0x15556]
    mipnp=[0x30,0xB0,0x2B0,0xAB0,0x2AB0,0xAAB0,0x2AAB0,0xAAAB0]
    im=Image.new('RGB',(su,sv)); px=im.load()
    def conv(v):
        if pf==1: return (((v>>11)&31)<<3,((v>>5)&63)<<2,(v&31)<<3)
        if pf==2: return (((v>>8)&15)*17,((v>>4)&15)*17,(v&15)*17)
        if pf==0: return (((v>>10)&31)<<3,((v>>5)&31)<<3,(v&31)<<3)
        return (255,0,255)
    if vq:
        idxbase=base+0x800+(mipvq[(tsp>>3)&7 if mip else sz] if mip else 0)
        for y in range(sv):
            for x in range(su):
                idx=rb(idxbase+(DIL[x>>1]<<1)+DIL[y>>1])
                px[x,y]=conv(r16(base+8*idx+((DIL[x&1]<<1)+DIL[y&1])*2))
    else:
        ab=base+(mipnp[(tsp>>3)&7] if mip else 0)
        for y in range(sv):
            for x in range(su):
                px[x,y]=conv(r16(ab+((DIL[x]<<1)+DIL[y])*2))
    return im,(mip,vq,pf,scan)
a=start; seen={}
while True:
    w=rd(a)
    if w==0 or w==0xdeadbeef: break
    if w>>31:
        tsp,tex=rd(a+16),rd(a+20)
        if (tex,tsp) not in seen and not (tex>>26)&1: seen[(tex,tsp)]=1
        a+=24
    else: a+=4
    if a>start+0x6000*4: break
keys=list(seen)[:72]
sheet=Image.new('RGB',(12*80,6*96),(0,0,0)); dr=ImageDraw.Draw(sheet)
for i,(tex,tsp) in enumerate(keys):
    try: im,info=decode(tex,tsp)
    except Exception as e: continue
    im=im.resize((76,76)); x,y=(i%12)*80,(i//12)*96
    sheet.paste(im,(x,y)); dr.text((x,y+78),'%d%d%d %x'%(info[0],info[1],info[2],tex&0x1fffff),fill=(255,255,255))
sheet.save(sys.argv[3]); print(len(seen),'distinct textures; showing',len(keys))
