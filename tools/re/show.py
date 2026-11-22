import sys; sys.path.insert(0,'.')
import xref
a0=int(sys.argv[1],16); a1=int(sys.argv[2],16)
i0=next(i for i,(a,_) in enumerate(xref.lines) if a>=a0); i1=next(i for i,(a,_) in enumerate(xref.lines) if a>=a1)
xref.show(i0,i1)
