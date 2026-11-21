"""Reference check for docs/model.md sections 4-7 and 11 (pure Python, no dependencies).

Topology only: neighbour rings as lists, counter-clockwise seen from outside.
  check(R)        the invariant checker (model.md section 11, checks 1-5)
  split(R,v,i,j)  the vertex split exactly as written in model.md section 6
Running this file tests every (i, j) split on an icosahedron and a tetrahedron, 25,000 random
splits, and the parallel selection rule of section 7 (winners pairwise at distance >= 3).
Use it as an oracle when writing the C++ and CUDA versions.
"""
import math, random, itertools, copy
from collections import deque

def icosa():
    t=(1+5**0.5)/2
    V=[(-1,t,0),(1,t,0),(-1,-t,0),(1,-t,0),(0,-1,t),(0,1,t),(0,-1,-t),(0,1,-t),(t,0,-1),(t,0,1),(-t,0,-1),(-t,0,1)]
    F=[(0,11,5),(0,5,1),(0,1,7),(0,7,10),(0,10,11),(1,5,9),(5,11,4),(11,10,2),(10,7,6),(7,1,8),
       (3,9,4),(3,4,2),(3,2,6),(3,6,8),(3,8,9),(4,9,5),(2,4,11),(6,2,10),(8,6,7),(9,8,1)]
    return [list(v) for v in V], F

def sub(a,b): return [a[i]-b[i] for i in range(3)]
def cross(a,b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def dot(a,b): return sum(x*y for x,y in zip(a,b))

def rings_ccw_outside(P):
    # neighbours = nearest 5 for icosahedron; sort CCW around outward radial direction
    n=len(P); R=[]
    for v in range(n):
        d=sorted((dot(sub(P[u],P[v]),sub(P[u],P[v])),u) for u in range(n) if u!=v)
        nb=[u for _,u in d[:5]]
        N=P[v]; ref=sub(P[nb[0]],P[v])
        # tangent basis: e1 = ref projected, e2 = N x e1  (CCW seen from outside)
        k=dot(ref,N)/dot(N,N); e1=[ref[i]-k*N[i] for i in range(3)]; e2=cross(N,e1)
        def ang(u):
            w=sub(P[u],P[v]); return math.atan2(dot(w,e2),dot(w,e1))
        nb.sort(key=ang); R.append(nb)
    return R

def check(R):
    V=len(R)
    for v in range(V):
        r=R[v]; d=len(r)
        assert d>=3, ("deg",v,d)
        assert v not in r and len(set(r))==d, ("dup/self",v,r)
        for k in range(d):
            a=r[k]; b=r[(k+1)%d]
            assert v in R[a], ("sym",v,a)
            assert b in R[a], ("consecutive not linked",v,a,b)
            ra=R[a]; ib=ra.index(b)
            assert ra[(ib+1)%len(ra)]==v, ("orientation",v,a,b,ra)
    assert sum(len(r) for r in R)==6*V-12, ("euler",sum(len(r) for r in R),V)
    seen={0}; q=deque([0])
    while q:
        x=q.popleft()
        for y in R[x]:
            if y not in seen: seen.add(y); q.append(y)
    assert len(seen)==V, "connected"

def split(R,v,i,j):
    """model.md section 6. i != j ring indices of v."""
    r=R[v]; d=len(r)
    r=r[i:]+r[:i]; j=(j-i)%d; i=0          # rotate so i = 0 < j
    a=r[0]; b=r[j]
    K=r[0:j+1]; M=r[j:]+[r[0]]
    vp=len(R)
    R[v]=K+[vp]
    R.append(M+[v])
    for w in M[1:-1]:
        rw=R[w]; rw[rw.index(v)]=vp
    ra=R[a]; ra.insert(ra.index(v)+1, vp)      # after v
    rb=R[b]; rb.insert(rb.index(v), vp)        # before v
    return vp

def newell_outward(P,R):
    for v in range(len(R)):
        r=R[v]; s=[0,0,0]
        for k in range(len(r)):
            c=cross(sub(P[r[k]],P[v]),sub(P[r[(k+1)%len(r)]],P[v])); s=[s[i]+c[i] for i in range(3)]
        assert dot(s,P[v])>0, ("normal not outward",v)

def main():
    P,F=icosa(); R=rings_ccw_outside(P); check(R); newell_outward(P,R)
    # convention check: (v, r[k], r[k+1]) are faces of the icosahedron (as cyclic triples, CCW outside)
    Fs={tuple(f[i:]+f[:i]) for f in map(list,F) for i in range(3)}
    Fs={tuple(x) for x in Fs}
    for v in range(12):
        for k in range(5):
            assert (v,R[v][k],R[v][(k+1)%5]) in Fs
    print("icosahedron ok: rings CCW from outside, Newell normal outward, faces match")

    # every (i,j) pair on small meshes
    base=R
    cnt=0
    for v in range(12):
        for i in range(5):
            for j in range(5):
                if i==j: continue
                R2=copy.deepcopy(base); split(R2,v,i,j); check(R2); cnt+=1
    print("all single splits ok:",cnt)

    # tetrahedron (deg 3 everywhere)
    T=[[1,2,3],[0,3,2],[0,1,3],[0,2,1]]
    check(T)
    for v in range(4):
        for i in range(3):
            for j in range(3):
                if i!=j:
                    T2=copy.deepcopy(T); split(T2,v,i,j); check(T2)
    print("tetrahedron splits ok")

    # long random sequence
    random.seed(1); R3=copy.deepcopy(base); maxdeg=0
    for step in range(20000):
        v=random.randrange(len(R3)); d=len(R3[v]); i=random.randrange(d); j=(i+d//2)%d
        if j==i: j=(i+1)%d
        split(R3,v,i,j)
        if step%500==0: check(R3)
        maxdeg=max(maxdeg,max(len(R3[x]) for x in (v,len(R3)-1)))
    check(R3); print("20000 random balanced splits ok, V =",len(R3),"max deg seen:",max(len(r) for r in R3))

    # arbitrary (unbalanced) pairs
    random.seed(2); R4=copy.deepcopy(base)
    for step in range(5000):
        v=random.randrange(len(R4)); d=len(R4[v]); i,j=random.sample(range(d),2)
        split(R4,v,i,j)
        if step%250==0: check(R4)
    check(R4); print("5000 random arbitrary splits ok, max deg:",max(len(r) for r in R4))

    # parallel selection: winners pairwise at distance >= 3, and simultaneous application is conflict-free
    random.seed(3); R5=copy.deepcopy(R3)
    V=len(R5)
    cand=[random.random()<0.3 for _ in range(V)]
    prio=[(random.getrandbits(32),v) if cand[v] else (-1,-1) for v in range(V)]
    m1=[max([prio[v]]+[prio[u] for u in R5[v]]) for v in range(V)]
    m2=[max([m1[v]]+[m1[u] for u in R5[v]]) for v in range(V)]
    win=[v for v in range(V) if cand[v] and prio[v]==m2[v]]
    def dist_le2(u,v):
        if u in R5[v]: return True
        return bool(set(R5[u])&set(R5[v]))
    ws=set(win)
    for v in win:
        near={v}|set(R5[v])
        near2=set(near)
        for x in near: near2|=set(R5[x])
        assert not (near2-{v})&ws, "two winners within distance 2"
    touched=set()
    for v in win:
        t={v}|set(R5[v]); assert not (t&touched); touched|=t
    for v in win:
        d=len(R5[v]); i=0; j=d//2
        split(R5,v,i,j)
    check(R5); print("parallel selection ok: winners",len(win),"of",sum(cand),"candidates; write sets disjoint; mesh valid")


if __name__ == '__main__':
    main()
