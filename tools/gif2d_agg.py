import sys,collections
d=collections.defaultdict(lambda:[0,0])
for f in sys.argv[1:]:
    for l in open(f,errors='replace'):
        if not l.startswith('H\t'): continue
        _,c,k,p,x=l.rstrip('\n').split('\t')
        d[(c,k)][0]+=int(p); d[(c,k)][1]+=int(x)
for (c,k),(p,x) in sorted(d.items(), key=lambda t:(t[0][0],-t[1][0],-t[1][1])):
    print(f"{c}\t{p}\t{x}\t{k}")
