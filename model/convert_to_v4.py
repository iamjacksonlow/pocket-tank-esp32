#!/usr/bin/env python3
"""Re-encode the v2 and v3 datasets as v4 lines (schema.md v4). Stdlib only.

Old datasets don't expire with a schema change (the v3m lesson, docs/stats.md):
the personality cliffs the shipped model has come from the v2 labels. v4 drops
the shadow field and adds `bored`, so:
  - `fish <name> ` and the friend name go (v2 lines), `trust 5` is inserted
    where a v2 line has none (as run_v3m_mixed.sh did);
  - the `shadow ...` field is removed. Any pair whose state HAD a shadow in
    view is dropped (its label may have been shaded by a predator the v4 line
    can no longer show), and so is every flee_shadow label;
  - `bored <0-2>` is inserted after trust: these labels were made with no
    notion of boredom, under "keep the last goal while it makes sense", which
    is exactly the fresh-fish rule of the v4 prompt. Never higher - bored 3-9
    behaviour comes only from v4 teacher data.

  python3 convert_to_v4.py out/v2_clean.jsonl out/v3_clean.jsonl --out out/v2v3_as_v4.jsonl
"""
import argparse
import json
import random
import re


def to_v4(state, rng):
    s = re.sub(r"^fish \w+ ", "", state)                                # v2: drop the name
    if " trust " not in s:
        s = re.sub(r"stage (\w+) ", r"stage \1 trust 5 ", s, count=1)   # v2: neutral trust
    s = re.sub(r"friend \w+ (near|mid|far) ", r"friend \1 ", s)         # v2: drop the friend name
    m = re.search(r" shadow (none|near \d+|mid \d+|far \d+) ", s)
    if not m:
        return None
    if m.group(1) != "none":
        return None                                                     # a predator shaded this label
    s = s[:m.start()] + " " + s[m.end():]
    s = re.sub(r"trust (\d) ", rf"trust \1 bored {rng.randint(0, 2)} ", s, count=1)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=4)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    kept = shadow = flee = bad = 0
    with open(args.out, "w") as out:
        for path in args.inputs:
            for line in open(path):
                try:
                    o = json.loads(line)
                except ValueError:
                    bad += 1
                    continue
                if o["goal"].startswith("flee_shadow"):
                    flee += 1
                    continue
                s = to_v4(o["state"], rng)
                if s is None:
                    shadow += 1
                    continue
                out.write(json.dumps({"state": s, "goal": o["goal"]}) + "\n")
                kept += 1
    print(f"kept {kept} v4 pairs -> {args.out}; dropped {shadow} with a shadow in view, {flee} flee labels, {bad} malformed")


if __name__ == "__main__":
    main()
