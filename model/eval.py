#!/usr/bin/env python3
"""Evaluate the exported student model against fresh sim states. Stdlib only.

Samples never-seen states from the same headless sim as gen_traces.py, runs the
native llama2.c binary on each, and reports what the student decided. With
--teacher it also asks Ollama for its goal on the same states and reports
student/teacher agreement (top-line distillation metric).

  python3 eval.py --count 20                 # student only, prints a table
  python3 eval.py --count 50 --teacher       # adds gemma4:26b agreement
"""

import argparse
import os
import random
import re
import subprocess
import sys

import gen_traces as gt

HERE = os.path.dirname(os.path.abspath(__file__))


def student_goal(run_bin, model_bin, tok_bin, state):
    prompt = state + " ->"
    out = subprocess.run(
        [run_bin, model_bin, "-z", tok_bin, "-t", "0", "-i", prompt],
        capture_output=True, text=True, timeout=60).stdout
    completion = out.split("->", 1)[1] if "->" in out else out
    m = re.search(r"([a-z_]+) urgency (\d)", completion)
    return f"{m.group(1)} urgency {m.group(2)}" if m else f"<unparsed: {completion.strip()[:40]}>"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--teacher", action="store_true", help="also query Ollama and score agreement")
    ap.add_argument("--host", default="http://192.168.0.139:11434")
    ap.add_argument("--model", default="gemma4:26b")
    ap.add_argument("--run-bin", default=os.path.join(HERE, "runw"))
    ap.add_argument("--model-bin", default=os.path.join(HERE, "out", "model.bin"))
    ap.add_argument("--tok-bin", default=os.path.join(HERE, "out", "tokenizer.bin"))
    ap.add_argument("--schema", type=int, choices=(2, 3, 4), default=2, help="state line schema of the model under test")
    args = ap.parse_args()
    gt.SCHEMA = args.schema

    for p in (args.run_bin, args.model_bin, args.tok_bin):
        if not os.path.exists(p):
            sys.exit(f"missing {p} (build/train/export first)")

    rng = random.Random(args.seed)
    tank = gt.Tank(rng)
    for _ in range(200):
        tank.tick()

    agree = valid = 0
    for i in range(args.count):
        for _ in range(40):
            tank.tick()
        fish = tank.fish[i % len(tank.fish)]
        if rng.random() < 0.45:
            tank.perturb(fish)
        state = gt.encode(tank, fish)
        s_goal = student_goal(args.run_bin, args.model_bin, args.tok_bin, state)
        if not s_goal.startswith("<"):
            valid += 1
        line = f"[{i+1:3d}] {s_goal:28s}"
        if args.teacher:
            t_goal = gt.ask_ollama(args.host, args.model, state, 30) or "<teacher failed>"
            hit = s_goal.split()[0] == t_goal.split()[0]
            agree += hit
            line += f" teacher: {t_goal:28s} {'MATCH' if hit else 'diff'}"
        print(line + f"  | {state}")
        if not s_goal.startswith("<"):
            fish.goal = s_goal.split()[0]

    print(f"\nvalid output: {valid}/{args.count}")
    if args.teacher:
        print(f"goal agreement with teacher: {agree}/{args.count} ({100*agree/args.count:.0f}%)")


if __name__ == "__main__":
    main()
