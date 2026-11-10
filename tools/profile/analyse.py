import sys,re,subprocess,collections,bisect
path=sys.argv[1]; exe=sys.argv[2]
L=open(path).read().split('\n'); n=int(L[0]); recs=[tuple(int(x,16) for x in l.split()) for l in L[1:n+1]]
maps=[]
for l in L[n+2:]:
    m=re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) (\S+) \S+ \S+\s*(.*)',l)
    if m: maps.append((int(m[1],16),int(m[2],16),m[3],int(m[4],16),m[5] or '[anon]'))
def where(a):
    for lo,hi,p,off,name in maps:
        if lo<=a<hi: return name.split('/')[-1],a-lo+off
    return '?',a
dso=collections.Counter(where(r[0])[0] for r in recs); t=len(recs)
print('samples',t); 
for k,v in dso.most_common(8): print(f'{100*v/t:5.1f}% {k}')
syms=[]
for l in subprocess.run(['nm','-C','--defined-only','-n',exe],capture_output=True,text=True).stdout.split('\n'):
    p=l.split(' ',2)
    if len(p)==3 and p[1] in 'tTwW':
        try: syms.append((int(p[0],16),p[2]))
        except: pass
syms.sort(); addrs=[s[0] for s in syms]
exelo=min(m[0]-m[3] for m in maps if m[4].endswith('rc_revenge_pro'))
def sym(a):
    if not a: return '(none)'
    i=bisect.bisect_right(addrs,a-exelo)-1
    return syms[i][1][:88] if i>=0 else hex(a)
print('--- by game-code frame (self or caller of a library)')
c=collections.Counter(sym(r[1]) for r in recs if not where(r[0])[0].startswith('libc'))
print('(excluding libc)')
for k,v in c.most_common(22): print(f'{100*v/t:5.1f}% {k}')

print('--- libc callers')
c=collections.Counter(sym(r[1]) for r in recs if where(r[0])[0].startswith('libc'))
for k,v in c.most_common(10): print(f'{100*v/t:5.1f}% {k}')
print('--- libc functions')
libc=[m for m in maps if m[4].endswith('libc.so.6')][0][4]
ls=[]
for l in subprocess.run(['nm','-D','--defined-only','-n',libc],capture_output=True,text=True).stdout.split('\n'):
    p=l.split()
    if len(p)==3:
        try: ls.append((int(p[0],16),p[2]))
        except: pass
ls.sort(); la=[x[0] for x in ls]
c=collections.Counter()
for r in recs:
    w=where(r[0])
    if w[0].startswith('libc'):
        i=bisect.bisect_right(la,w[1])-1; c[(ls[i][1] if i>=0 else '?',sym(r[1])[:40])]+=1
for k,v in c.most_common(10): print(f'{100*v/t:5.1f}% {k[0]} <- {k[1]}')
print('--- vdso callers')
c=collections.Counter(sym(r[1])[:60] for r in recs if where(r[0])[0]=='[vdso]')
for k,v in c.most_common(5): print(f'{100*v/t:5.1f}% {k}')
