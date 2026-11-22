"""Cross-reference strings to code in a disassembled ESP32-S3 app image.

Run in a directory holding seg_*.bin (from split_image.py) and irom.S/iram0.S/iram1.S
(objdump output). Usage: xref.py "log message" ...  -> prints the functions that load
the string. show.py <start> <end> prints a range with literal values and strings resolved.
Segment addresses/file names match the stock Holoneo image; adjust for other images.
"""
import struct, re, sys, bisect
segs={}
import glob
for f in glob.glob('seg_*.bin'):
    a=int(f[4:12],16); segs[a]=open(f,'rb').read()
def find_str(s):
    out=[]
    for a,b in segs.items():
        i=b.find(s.encode())
        while i!=-1:
            st=b.rfind(b'\0',0,i)+1
            out.append(a+st); i=b.find(s.encode(),i+1)
    return out
def find_word(v):
    out=[]; p=struct.pack('<I',v)
    for a,b in segs.items():
        i=b.find(p)
        while i!=-1:
            if i%4==0: out.append(a+i)
            i=b.find(p,i+1)
    return out
lines=[]
for f in ['irom.S','iram0.S','iram1.S']:
    for ln in open(f):
        m=re.match(r'\s*([0-9a-f]{8}):\s+(?:[0-9a-f]{2}\s?)+\s+(\S.*)',ln)
        if m: lines.append((int(m.group(1),16),m.group(2).strip()))
lines.sort()
addrs=[a for a,_ in lines]
l32r_by_target={}
for i,(a,ins) in enumerate(lines):
    m=re.match(r'l32r\s+a\d+,\s*(?:0x)?([0-9a-f]+)',ins)
    if m: l32r_by_target.setdefault(int(m.group(1),16),[]).append(i)
def func_bounds(i):
    s=i
    while s>0 and not lines[s][1].startswith('entry') : s-=1
    e=i
    while e<len(lines)-1 and not lines[e][1].startswith('retw') : e+=1
    return s,e
def lit_val(t):
    for a,b in segs.items():
        if a<=t<a+len(b)-3: return struct.unpack_from('<I',b,t-a)[0]
def show(i0,i1):
    for a,ins in lines[i0:i1+1]:
        extra=''
        m=re.match(r'l32r\s+a\d+,\s*(?:0x)?([0-9a-f]+)',ins)
        if m:
            v=lit_val(int(m.group(1),16))
            if v is not None:
                extra=f'   ; ={v:#x}'
                if 0x3c160020<=v<0x3c2c0000:
                    b=segs[0x3c160020]; o=v-0x3c160020; s=b[o:b.find(b"\0",o)][:60]
                    extra+=f' "{s.decode("latin-1")}"'
        print(f'{a:08x}: {ins}{extra}')
if __name__=='__main__':
    for s in sys.argv[1:]:
        for sa in find_str(s):
            for la in find_word(sa):
                for i in l32r_by_target.get(la,[]):
                    st,en=func_bounds(i)
                    print(f'===== "{s}" str@{sa:#x} lit@{la:#x} used@{lines[i][0]:#x} func {lines[st][0]:#x}-{lines[en][0]:#x} ({en-st} ins)')
