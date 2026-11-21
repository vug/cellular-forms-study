"""How the choice of cleavage pair affects neighbour counts (model.md section 6, degree control).

Topology only. Cells divide at random, food-driven; compare picking the cleavage index at random
with picking the near-opposite pair whose two end cells have the fewest links.
"""
import random, copy, collections
from split_check import icosa, rings_ccw_outside, split, check
P,F=icosa(); base=rings_ccw_outside(P)

def run(policy, n=40000, seed=1, cap=None):
    random.seed(seed); R=copy.deepcopy(base); postponed=0
    food=[random.random() for _ in R]
    done=0
    while done<n:
        # food-driven: each cell accumulates random food; split when >1 (random-order emulation)
        v=random.randrange(len(R))
        food[v]+=random.random()*0.5
        if food[v]<1: continue
        d=len(R[v]); r=R[v]
        pairs=[]
        for i in range(d):
            for off in policy['offs'](d):
                j=(i+off)%d
                if j==i: continue
                a,b=r[i],r[j]
                kd=(j-i)%d+2; md=d-((j-i)%d)+2
                pairs.append((i,j,len(R[a]),len(R[b]),kd,md))
        if cap: pairs=[p for p in pairs if p[2]+1<=cap and p[3]+1<=cap and p[4]<=cap and p[5]<=cap]
        if not pairs: postponed+=1; food[v]=0.9; continue
        if policy['pick']=='random': i,j=random.choice(pairs)[:2]
        elif policy['pick']=='mindeg':
            best=min(max(p[2],p[3])*100+p[2]+p[3] for p in pairs)
            i,j=random.choice([p for p in pairs if max(p[2],p[3])*100+p[2]+p[3]==best])[:2]
        vp=split(R,v,i,j); food[v]/=2; food.append(food[v]); done+=1
    check(R)
    h=collections.Counter(len(r) for r in R)
    return max(h), dict(sorted(h.items())), postponed

half=lambda d:[d//2]
half_pm=lambda d:sorted({d//2,(d+1)//2})
print("random i, j=i+d/2            :", run({'offs':half,'pick':'random'})[:2])
print("min-degree endpoints, opposite:", run({'offs':half_pm,'pick':'mindeg'})[:2])
print("random + cap 12 (postpone)    :", run({'offs':half,'pick':'random'},cap=12))
print("mindeg + cap 12               :", run({'offs':half_pm,'pick':'mindeg'},cap=12))
