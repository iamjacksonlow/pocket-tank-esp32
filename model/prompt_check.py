#!/usr/bin/env python3
"""Check a teacher prompt BEFORE the overnight run. Stdlib only.

The v3 overnight taught us the expensive way that one sentence in the teacher
prompt reshapes every label (P(follow | social>=8) fell 0.50 -> 0.18). This
asks the teacher ~300 questions on a fixed diagnostic panel and prints the
conditional rates the student will inherit - ~7 minutes with 3 workers, not
8 hours. Use it to compare prompts, or teachers (--model qwen3.5:27b ...).

  python3 prompt_check.py --schema 3                       # current v3 prompt
  python3 prompt_check.py --schema 3 --prompt-file cand.txt # a candidate prompt
  python3 prompt_check.py --schema 3 --model qwen3.5:27b   # another teacher

Panel (per schema line, identity randomized unless the case pins it):
  social9   content fish, social 9 (expect follow_friend high)     x40
  social1   content fish, social 1 (expect follow_friend ~0)        x40
  bold9     content + energetic, bold 9 (expect dart_play present)  x40
  bold1     same, bold 1 (expect dart_play ~0)                      x40
  content   content, mid identity (watch for a single attractor)    x40
  shadowcalm shadow near, stress 0-3 (expect flee high)             x40
  lonely    friend none, content (must not flee / seek food)        x30
  starving  hunger 9, food near (expect seek_food ~1.0)             x30
  trust     content, trust 9 vs trust 0 (v3 only; expect a shade, not a flip) x40
  v4 (--schema 4; the shadow cases are skipped, bored cases added):
  bored9bub  content, bored 9, last visit_bubbles (expect a change, explore up) x40
  bored9fol  social 8, friend near, bored 9, last follow_friend (expect a change) x40
  bored0bub  content, bored 0, last visit_bubbles (expect last mostly KEPT)     x40
  nightbored night, bored 9, last rest (expect rest - boredom never wakes the tank) x30
"""
import argparse
import concurrent.futures as cf
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_traces as gt  # noqa: E402


def state(rng, schema, **pin):
    f = dict(zone=rng.randint(1, 6), hunger=rng.randint(0, 3), energy=rng.randint(5, 9), stress=rng.randint(0, 2),
             curiosity=rng.randint(3, 8), bold=rng.randint(2, 7), social=rng.randint(2, 6),
             stage=rng.choice(["juv", "adult", "adult", "elder"]), trust=rng.randint(2, 7),
             bored=rng.randint(0, 2), food="none", shadow="none", friend=f"{rng.choice(['near','mid'])} {rng.randint(1,12)}",
             bubble=f"{rng.choice(['near','mid','far'])} {rng.randint(1,12)}",
             reef=f"{rng.choice(['mid','far'])} {rng.randint(1,12)}", wall="clear",
             last=rng.choice(["explore", "inspect_reef", "visit_bubbles", "rest"]), time="day")
    f.update(pin)
    ident = f"hunger {f['hunger']} energy {f['energy']} stress {f['stress']} curiosity {f['curiosity']} bold {f['bold']} social {f['social']} stage {f['stage']}"
    tail = (f"food {f['food']} shadow {f['shadow']} friend {f['friend']} bubble {f['bubble']} reef {f['reef']} "
            f"wall {f['wall']} last {f['last']} time {f['time']}")
    if schema >= 4:
        return (f"zone {f['zone']} {ident} trust {f['trust']} bored {f['bored']} food {f['food']} friend {f['friend']} "
                f"bubble {f['bubble']} reef {f['reef']} wall {f['wall']} last {f['last']} time {f['time']}")
    if schema >= 3:
        return f"zone {f['zone']} {ident} trust {f['trust']} {tail}"
    name = rng.choice(["mira", "bolt", "kelp", "nori"])
    fr = f['friend'] if f['friend'] == "none" else f"{rng.choice(['mira','bolt','kelp','nori'])} {f['friend']}"
    return f"fish {name} zone {f['zone']} {ident} {tail.replace('friend ' + f['friend'], 'friend ' + fr, 1)}"


def panel(rng, schema, n):
    P = []
    def add(case, k, **pin):
        for _ in range(int(n * k / 40)):
            P.append((case, state(rng, schema, **pin)))
    add("social9", 40, social=9)
    add("social1", 40, social=1)
    add("bold9", 40, bold=9, energy=9, hunger=rng.randint(0, 2))
    add("bold1", 40, bold=1, energy=9, hunger=rng.randint(0, 2))
    add("content", 40)
    if schema < 4:
        add("shadowcalm", 40, shadow=f"near {rng.randint(1,12)}", stress=rng.randint(0, 3))
    add("lonely", 30, friend="none")
    add("starving", 30, hunger=9, food=f"near {rng.randint(1,12)}")
    if schema >= 3:
        add("trust9", 20, trust=9)
        add("trust0", 20, trust=0)
    if schema >= 4:
        add("bored9bub", 40, bored=9, last="visit_bubbles", bubble=f"near {rng.randint(1,12)}")
        add("bored9fol", 40, bored=9, last="follow_friend", social=8, friend=f"near {rng.randint(1,12)}")
        add("bored0bub", 40, bored=0, last="visit_bubbles", bubble=f"near {rng.randint(1,12)}")
        add("nightbored", 30, bored=9, last="rest", time="night", energy=rng.randint(3, 6), reef=f"near {rng.randint(1,12)}")
    return P


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schema", type=int, choices=(2, 3, 4), default=3)
    ap.add_argument("--host", default="http://192.168.0.139:11434")
    ap.add_argument("--model", default="gemma4:26b")
    ap.add_argument("--prompt-file", default=None, help="candidate system prompt (default: gen_traces' prompt for the schema)")
    ap.add_argument("--n", type=int, default=40, help="calls per major case (default 40 -> ~320 calls)")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()
    gt.SCHEMA = args.schema
    if args.prompt_file:
        txt = open(args.prompt_file).read().strip()
        gt.SYSTEM_PROMPT = txt
        gt.SYSTEM_PROMPT_V3 = txt
        gt.SYSTEM_PROMPT_V4 = txt
    rng = random.Random(args.seed)
    P = panel(rng, args.schema, args.n)
    print(f"{len(P)} calls to {args.model} @ {args.host} ({args.workers} workers) ...", flush=True)

    def ask(item):
        case, st = item
        try:
            g = gt.ask_ollama(args.host, args.model, st, 30)
        except Exception as e:  # noqa: BLE001
            g = None
        return case, (g.split()[0] if g else None)

    results = {}
    fails = 0
    with cf.ThreadPoolExecutor(args.workers) as ex:
        for case, goal in ex.map(ask, P):
            if goal is None:
                fails += 1
                continue
            results.setdefault(case, {}).setdefault(goal, 0)
            results[case][goal] += 1

    def rate(case, goal):
        d = results.get(case, {}); n = sum(d.values())
        return (d.get(goal, 0) / n if n else float("nan")), n

    def top(case):
        d = results.get(case, {}); n = sum(d.values()) or 1
        return ", ".join(f"{g} {c/n:.2f}" for g, c in sorted(d.items(), key=lambda kv: -kv[1])[:3])

    print(f"\nfailures: {fails}")
    checks = [
        ("P(follow | social 9)", *rate("social9", "follow_friend"), ">= 0.40", lambda v: v >= 0.40),
        ("P(follow | social 1)", *rate("social1", "follow_friend"), "<= 0.05", lambda v: v <= 0.05),
        ("P(dart | bold 9, energetic)", *rate("bold9", "dart_play"), ">= 0.10", lambda v: v >= 0.10),
        ("P(dart | bold 1, energetic)", *rate("bold1", "dart_play"), "<= 0.03", lambda v: v <= 0.03),
        ("P(seek_food | starving)", *rate("starving", "seek_food"), ">= 0.95", lambda v: v >= 0.95),
        ("P(seek_food | lonely, not hungry)", *rate("lonely", "seek_food"), "<= 0.05", lambda v: v <= 0.05),
    ]
    if args.schema < 4:
        checks += [
            ("P(flee | shadow near, calm)", *rate("shadowcalm", "flee_shadow"), ">= 0.80", lambda v: v >= 0.80),
            ("P(flee | lonely, no shadow)", *rate("lonely", "flee_shadow"), "== 0", lambda v: v <= 0.02),
        ]
    else:
        checks += [
            ("P(bubbles again | bored 9, last bubbles)", *rate("bored9bub", "visit_bubbles"), "<= 0.15", lambda v: v <= 0.15),
            ("P(explore | bored 9, last bubbles)", *rate("bored9bub", "explore"), ">= 0.25", lambda v: v >= 0.25),
            ("P(follow again | bored 9, last follow)", *rate("bored9fol", "follow_friend"), "<= 0.15", lambda v: v <= 0.15),
            ("P(bubbles kept | bored 0, last bubbles)", *rate("bored0bub", "visit_bubbles"), ">= 0.40", lambda v: v >= 0.40),
            ("P(rest | night, bored 9, last rest)", *rate("nightbored", "rest"), ">= 0.70", lambda v: v >= 0.70),
            ("P(flee | any v4 case)", (sum(d.get("flee_shadow", 0) for d in results.values()) / max(1, sum(sum(d.values()) for d in results.values()))), sum(sum(d.values()) for d in results.values()), "== 0", lambda v: v <= 0.001),
        ]
    for name, v, n, want, ok in checks:
        print(f"  {'OK ' if ok(v) else 'BAD'}  {name:36s} = {v:.2f}  (n={n}, want {want})")
    # attractor check: no single goal should own the content fish
    d = results.get("content", {}); n = sum(d.values()) or 1
    mx = max(d.values()) / n if d else 0
    print(f"  {'OK ' if mx < 0.45 else 'BAD'}  {'max single goal | content':36s} = {mx:.2f}  (want < 0.45)   [{top('content')}]")
    if args.schema >= 3:
        print(f"       trust 9: [{top('trust9')}]\n       trust 0: [{top('trust0')}]")
    print("\n  per case:")
    for case in ("social9", "social1", "bold9", "bold1", "content", "shadowcalm", "lonely", "starving", "trust9", "trust0",
                 "bored9bub", "bored9fol", "bored0bub", "nightbored"):
        if case in results:
            print(f"    {case:10s} {top(case)}")


if __name__ == "__main__":
    main()
