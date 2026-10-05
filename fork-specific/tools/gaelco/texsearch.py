#!/usr/bin/env python3
# usage: texsearch.py <gpuram.bin> <list_start> : brute-force VQ decode variants, score by smoothness
import struct, sys, itertools, collections
b_=None
def load(fn):
    d=open(fn,'rb').read(); pages={}; i=0
    while i<len(d):
        p=struct.unpack_from('<I',d,i)[0]; pages[p]=d[i+4:i+4+4096]; i+=4+4096
    return pages
b=load(sys.argv[1]); start=int(sys.argv[2],16)
def mem(a,n):
    out=bytearray()
    k=a&~0xfff
    while k<a+n:
        out+=b.get((k-0x18000000)>>12,b'\xde\xad\xbe\xef'*1024); k+=4096
    o=a&0xfff; return bytes(out[o:o+n])
def rd(a):
    pg=b.get((a-0x18000000)>>12); return struct.unpack_from('<I',pg,a&0xfff)[0] if pg else 0
DIL=[sum(((v>>i)&1)<<(2*i) for i in range(10)) for v in range(1024)]
# collect textures
a=start; tex=[]; seen=set()
while True:
    w=rd(a)
    if w==0 or w==0xdeadbeef: break
    if w>>31:
        tsp,t=rd(a+16),rd(a+20)
        if (t,tsp) not in seen and (t>>30)&1 and ((t>>27)&7)==1:
            seen.add((t,tsp)); tex.append((t,tsp))
        a+=24
    else: a+=4
print(len(tex),'VQ565 textures')
def rough(img,w,h):
    s=0
    for y in range(h-1):
        for x in range(w-1):
            p=img[y*w+x]; q=img[y*w+x+1]; r=img[(y+1)*w+x]
            s+=abs(((p>>11)&31)-((q>>11)&31))+abs(((p>>5)&63)-((q>>5)&63))//2+abs((p&31)-(q&31))
            s+=abs(((p>>11)&31)-((r>>11)&31))+abs(((p>>5)&63)-((r>>5)&63))//2+abs((p&31)-(r&31))
    return s/(w*h)
mipvq=[0x6,0x16,0x56,0x156,0x556,0x1556,0x5556,0x15556]
perms=list(itertools.permutations(range(4)))
res=collections.defaultdict(list)
for (t,tsp) in tex[:40]:
    mip=(t>>31)&1; su=(tsp>>3)&7; sv=tsp&7
    if su>4: continue   # keep it quick: <=256
    w=8<<su; h=8<<(su if mip else sv)
    base=0x19000000+((t&0x1fffff)<<3)
    n=0x800+(w*h//4)+0x20000
    data=mem(base,min(n,0x40000))
    cb=struct.unpack_from('<1024H',data,0)
    for offname,off in (('0x800+mip',0x800+(mipvq[su] if mip else 0)),('0x800',0x800)):
        for idxname in ('tw','tw_swap','lin'):
            for pi,pm in enumerate(perms):
                img=[0]*(w*h)
                ok=True
                for y in range(h):
                    for x in range(w):
                        if idxname=='tw': io=(DIL[x>>1]<<1)+DIL[y>>1]
                        elif idxname=='tw_swap': io=(DIL[y>>1]<<1)+DIL[x>>1]
                        else: io=(y>>1)*(w>>1)+(x>>1)
                        if off+io>=len(data): ok=False;break
                        idx=data[off+io]
                        sub=(y&1)*2+(x&1)
                        img[y*w+x]=cb[idx*4+pm[sub]]
                    if not ok: break
                if not ok: continue
                res[(offname,idxname,pm)].append(rough(img,w,h))
best=sorted(((sum(v)/len(v),len(v),k) for k,v in res.items()))[:8]
for s,n,k in best: print('%.2f over %d: %s'%(s,n,k))
