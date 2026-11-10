"""QA fuzzer for the launcher input mapping (run from the launcher branch).

Generates random valid control layouts, drives DraftInputEngine with random
key sequences and checks pointer invariants, then pushes the encoded events
through ScrcpyChannel over a local socket pair. No Android is involved; this
only shows the host-side mapping keeps its own invariants.

Usage (inside the launcher checkout, with the project installed):
    python launcher_input_fuzz.py 6000
"""

import random, socket, sys, itertools, collections
from lynxdroid.input.draft_mapping import draft_engine, DOUBLE_TAP_INTERVAL_MS
from lynxdroid.input.events import Action
from lynxdroid.input.scrcpy_touch import encode_touch
from lynxdroid.input.scrcpy_channel import ScrcpyChannel
from lynxdroid.core.errors import LynxError

KINDS = ["Нажатие","Двойное нажатие","Мультитач","Джойстик","Камера","Перетаскивание","Огонь"]
def controls(rng):
    out=[]
    pool=["q","e","r","t","f","g","h","z","x","c","v","b","1","2","3","Mouse1","Mouse2"]
    rng.shuffle(pool)
    n=rng.randint(1,14); joy=False
    for i in range(n):
        kind=rng.choice(KINDS)
        if kind=="Джойстик":
            if joy: kind="Огонь"
            else:
                joy=True
                out.append({"kind":kind,"key":"W,D,S,A","x":rng.uniform(0.3,0.7),"y":rng.uniform(0.3,0.7)}); continue
        out.append({"kind":kind,"key":pool[i],"x":rng.choice([0,1,rng.random()]),"y":rng.choice([0,1,rng.random()])})
    return out

fail=collections.Counter(); examples={}; stats=collections.Counter()
def check(seed):
    rng=random.Random(seed)
    cs=controls(rng)
    try:
        eng=draft_engine(cs)
    except ValueError as e:
        fail['__rejected__ '+str(e)[:50]]+=1; examples.setdefault('__rejected__ '+str(e)[:50],(seed,cs,[]))
        return
    a,b=socket.socketpair()
    ch=ScrcpyChannel(a,1920,1080)
    open_ptrs={}
    now=0
    allkeys=list("qertyfghzxcvb123wasdilkj8654")+["mouse1","mouse2"]
    log=[]; stats['built']+=1
    def feed(events, what):
        for ev in events:
            stats[ev.action.value]+=1
            log.append((what, ev.action.value, ev.pointer, round(ev.x,1), round(ev.y,1)))
            if not (0<=ev.x<1920 and 0<=ev.y<1080):
                raise AssertionError(f"out of range {ev}")
            if ev.action is Action.DOWN:
                if ev.pointer in open_ptrs: raise AssertionError(f"pointer reuse {ev.pointer}")
                open_ptrs[ev.pointer]=ev
            elif ev.action is Action.MOVE:
                if ev.pointer not in open_ptrs: raise AssertionError("move without down")
            else:
                if ev.pointer not in open_ptrs: raise AssertionError("up without down")
                del open_ptrs[ev.pointer]
            encode_touch(ev,1920,1080)
        if events:
            ch.send(list(events))
            b.setblocking(False)
            try:
                while b.recv(65536): pass
            except BlockingIOError: pass
    try:
        for step in range(rng.randint(5,80)):
            now+=rng.choice([0,1,5,20,119,120,121,500])
            op=rng.random()
            if op<0.4:
                k=rng.choice(allkeys); feed(eng.key_down(k,now_ms=now,repeat=rng.random()<0.1), f"down {k}")
            elif op<0.75:
                k=rng.choice(allkeys); feed(eng.key_up(k,now_ms=now), f"up {k}")
            elif op<0.9:
                feed(eng.mouse_delta(rng.uniform(-3000,3000),rng.uniform(-3000,3000),now_ms=now),"motion")
            elif op<0.97:
                feed(eng.tick(now_ms=now),"tick")
            else:
                feed(eng.release_all("focus lost"),"release_all")
                if open_ptrs or eng.active_pointers: raise AssertionError("contacts survive release_all")
        feed(eng.release_all("end"),"end")
        now+=1000
        feed(eng.tick(now_ms=now),"tick-after-end")
        if open_ptrs or eng.active_pointers: raise AssertionError(f"leaked contacts {open_ptrs} {eng.active_pointers}")
    except (AssertionError, LynxError, ValueError, KeyError) as e:
        key=type(e).__name__+": "+str(e)[:60]
        fail[key]+=1
        examples.setdefault(key,(seed,cs,log[-8:]))
    finally:
        try: ch.close()
        except Exception: pass
        b.close()
for s in range(int(sys.argv[1]) if len(sys.argv)>1 else 3000): check(s)
print("failures:",sum(fail.values()), dict(stats))
for k,v in fail.most_common(12):
    print(v,k); print("   seed",examples[k][0]); print("   controls",examples[k][1]); print("   log",examples[k][2])
