#!/usr/bin/env python3
import struct, sys
def load(fn):
    d=open(fn,'rb').read(); pages={}; i=0
    while i<len(d):
        p=struct.unpack_from('<I',d,i)[0]; pages[p]=d[i+4:i+4+4096]; i+=4+4096
    return pages
b=load(sys.argv[1]); start=int(sys.argv[2],16)
def mem(a,n):
    out=bytearray(); k=a&~0xfff
    while k<a+n:
        out+=b.get((k-0x18000000)>>12,b'\xde\xad\xbe\xef'*1024); k+=4096
    o=a&0xfff; return bytes(out[o:o+n])
def rd(a):
    pg=b.get((a-0x18000000)>>12); return struct.unpack_from('<I',pg,a&0xfff)[0] if pg else 0
DIL=[sum(((v>>i)&1)<<(2*i) for i in range(10)) for v in range(1024)]
a=start; seen={}
while True:
    w=rd(a)
    if w==0 or w==0xdeadbeef: break
    if w>>31:
        tsp,t=rd(a+16),rd(a+20)
        if (t,tsp) not in seen: seen[(t,tsp)]=1
        a+=24
    else: a+=4
mipvq=[0x6,0x16,0x56,0x156,0x556,0x1556,0x5556,0x15556]
mipnp=[0x30,0xB0,0x2B0,0xAB0,0x2AB0,0xAAB0,0x2AAB0,0xAAAB0]
def rough(img,w,h):
    s=0;n=0
    for y in range(0,h-1):
        for x in range(0,w-1):
            p=img[y*w+x]; q=img[y*w+x+1]; r=img[(y+1)*w+x]
            for c in (q,r):
                s+=abs(((p>>11)&31)-((c>>11)&31))+abs(((p>>5)&63)-((c>>5)&63))//2+abs((p&31)-(c&31))
            n+=1
    return s/n
rows=[]
for (t,tsp) in seen:
    mip=(t>>31)&1; vq=(t>>30)&1; pf=(t>>27)&7; scan=(t>>26)&1
    su=(tsp>>3)&7; sv=tsp&7
    if pf!=1 or su>5 or sv>5: continue
    if mip: sv=su
    w=8<<su; h=8<<sv
    base=0x19000000+((t&0x1fffff)<<3)
    d=mem(base,0x800+w*h*3+0x1000)
    if d[:4]==b'\xef\xbe\xad\xde': continue
    img=[0]*(w*h)
    if vq:
        cb=struct.unpack_from('<1024H',d,0); off=0x800+(mipvq[su] if mip else 0)
        for y in range(h):
            for x in range(w):
                img[y*w+x]=cb[d[off+(DIL[x>>1]<<1)+DIL[y>>1]]*4+((DIL[x&1]<<1)+DIL[y&1])]
    else:
        off=(mipnp[su] if mip else 0)
        for y in range(h):
            for x in range(w):
                o=off+((DIL[x]<<1)+DIL[y])*2; img[y*w+x]=d[o]|(d[o+1]<<8)
    rows.append((rough(img,w,h),w,h,mip,vq,scan,t&0x1fffff,t>>21&0x3f,tsp))
rows.sort()
print('rough  w   h  mip vq scan  addr    palsel  tsp')
for r in rows: print('%5.1f %4d %4d  %d   %d  %d   %06x  %02x  %08x'%r)
