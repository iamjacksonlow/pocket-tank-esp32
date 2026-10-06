#!/usr/bin/env python3
"""Trace generator for the pocket-tank advisor model.

Runs a minimal headless tank sim, encodes per-fish state per schema.md (DRAFT),
asks Ollama (gemma4:26b, structured output) for a goal, and appends
{"state": ..., "goal": ...} lines to a JSONL file.

  python3 gen_traces.py --count 10 --dry-run    # print encoded states, no network
  python3 gen_traces.py --count 1000            # query Ollama, append to out/traces.jsonl

Stdlib only. Schema v2 is FROZEN (see schema.md); `--schema 3` emits the v3 line
(no fish names, + trust 0-9, tanks of 2-6 fish); `--schema 4` (2026-09-14) drops
the shadow - the predator left the game on 2026-09-13 - and adds `bored 0-9`,
how stale the fish's current activity is, so the teacher can push a fish out of
a rut (the bubble-column and follow-the-friend loops Strato saw on the device).
Keep this encoder in exact sync with common/llm/advisor_core.c.
"""

import argparse
import json
import math
import os
import random
import subprocess
import sys
import time
import urllib.error
import urllib.request

TANK_W, TANK_H = 448, 368
FISH_NAMES = ["mira", "bolt", "kelp", "nori", "pip", "sol"]
SCHEMA = 2          # set by --schema; 3 = v3 line, 4 = v4 line (see schema.md)
GOALS = [
    "seek_food", "flee_shadow", "visit_bubbles", "follow_friend",
    "explore", "rest", "dart_play", "inspect_reef",
]
GOALS_V4 = [g for g in GOALS if g != "flee_shadow"]   # no shadow, nothing to flee
# the pastimes boredom accrues on (rest counts by day only; eating never)
LEISURE = ["visit_bubbles", "follow_friend", "explore", "dart_play", "inspect_reef"]
# boredom dynamics, per sim tick (~a frame); mirrors common/tank.c BORED_*
BORED_PER_TICK, BORED_NEW_GOAL, BORED_NEW_ZONE, BORED_RELIEF_PER_TICK = 0.08, 4.0, 1.5, 0.15


def goals():
    return GOALS_V4 if SCHEMA >= 4 else GOALS

# Distance buckets in pixels (tank diagonal ~580).
NEAR, MID, FAR = 70, 180, 380

SYSTEM_PROMPT = (
    "You are the sparse instinct advisor for a small aquarium fish. "
    "You receive one line describing the fish's identity and drives (0-9) and what "
    "it senses (distances none/near/mid/far, directions as clock positions relative "
    "to its heading, 12 = dead ahead). Choose the single most fitting goal and an "
    "urgency 0-9. The goals: seek_food (hungry and food exists), flee_shadow (a "
    "shadow looms, more urgent the closer it is), rest (low energy, or nighttime "
    "calm), follow_friend (sociable, a friend in view), visit_bubbles (relaxed fish "
    "enjoy playing in the bubble column), dart_play (high energy and playful, a "
    "joyful burst), inspect_reef (curious about the reef, especially when near it), "
    "explore (default wandering when nothing else calls). "
    "Identity shades every choice. bold: a bold fish (7-9) takes risks, plays, and "
    "shrugs off distant shadows; a timid fish (0-2) startles easily, flees sooner, "
    "and prefers the reef's safety. social: only a very social fish (7-9) follows "
    "friends now and then; below that, following a friend is rare - friends are "
    "usually nearby in a small tank, so mere proximity is never a reason. stage: "
    "fry stay near friends and the reef and are easily scared; juv are playful "
    "and curious; adult are balanced; elder rest more and play less. "
    "A shadow is a predator: when a shadow is near, fleeing beats everything, and "
    "no fish should seek food near or under a shadow - survival first. "
    "At night fish wind down and rest by the reef unless something urgent calls. "
    "A content fish, fed and safe, should vary between the playful and curious "
    "goals rather than always exploring. Keep the last goal only while it still "
    "makes sense: stop seeking food when hunger is low, stop fleeing when no "
    "shadow is in sight. Use the whole urgency range: 9 is life-or-death, 7-8 "
    "pressing, 4-6 a normal want, 1-3 mild, 0 an idle whim."
)

# v3 additions (trust, and friend none): appended to the system prompt for --schema 3
SYSTEM_PROMPT_V3 = SYSTEM_PROMPT + (
    " trust: how much the fish trusts its keeper (0-9). A trusting fish (7-9) is "
    "calmer - it recovers from a fright sooner, is comfortable near the surface, "
    "and is a little bolder about food and play; a wary fish (0-2) is jumpier - it "
    "flees at less, stays lower and nearer the reef, and takes longer to settle. "
    "Trust shades but never overrides hunger or a predator. If no friend is in "
    "view (friend none), following is impossible - choose something else."
)

# v4 (2026-09-14): the shadow is gone from the game, so the predator paragraph
# goes; `bored` arrives; explore is a real activity, not the fallback; stress
# gets its own meaning now that nothing looms. Identity wording stays the v2
# text that produced the personality cliffs v3m ships with (docs/stats.md:
# the v3 rewrite flattened them - prompt-is-DNA). Check with prompt_check.py
# before any overnight run.
SYSTEM_PROMPT_V4 = (
    "You are the sparse instinct advisor for a small aquarium fish. "
    "You receive one line describing the fish's identity and drives (0-9) and what "
    "it senses (distances none/near/mid/far, directions as clock positions relative "
    "to its heading, 12 = dead ahead). Choose the single most fitting goal and an "
    "urgency 0-9. The goals: seek_food (hungry and food exists), rest (low energy, "
    "or nighttime calm), follow_friend (sociable, a friend in view), visit_bubbles "
    "(relaxed fish enjoy playing in the bubble column), dart_play (high energy and "
    "playful, a joyful burst), inspect_reef (curious about the reef, especially when "
    "near it), explore (cruising off to a part of the tank the fish has not seen in "
    "a while - a real activity in its own right, not a fallback: a healthy fish "
    "spends a good share of its day exploring). "
    "Identity shades every choice. bold: a bold fish (7-9) takes risks, plays, and "
    "roams the open water; a timid fish (0-2) startles easily, prefers the reef's "
    "safety, and does not dart: dart_play is only ever chosen by a fish with bold 5 "
    "or more and energy to burn - a fish with bold 0-3 never picks it, however "
    "energetic. social: only a very social fish (7-9) follows "
    "friends now and then; below that, following a friend is rare - friends are "
    "usually nearby in a small tank, so mere proximity is never a reason. stage: "
    "fry stay near friends and the reef and are easily scared; juv are playful "
    "and curious; adult are balanced; elder rest more and play less. "
    "trust: how much the fish trusts its keeper (0-9). A trusting fish (7-9) is "
    "calmer - it recovers from a fright sooner, is comfortable near the surface, "
    "and is a little bolder about food and play; a wary fish (0-2) is jumpier - it "
    "stays lower and nearer the reef, and takes longer to settle. "
    "Trust shades but never overrides hunger. "
    "stress: a stressed fish (7-9) has been startled or crowded - it settles by "
    "the reef or rests until it calms, and does not play. "
    "bored: how stale the fish's current activity has become (0 = just started, "
    "9 = stuck in a rut). A fresh fish (0-2) naturally keeps its last goal while "
    "it still makes sense. As boredom rises the fish wants a change: at 5-6 "
    "repeating last is unlikely, and at 7-9 the fish must choose something "
    "different from last - the more bored it is, the more it favors explore, "
    "which takes it somewhere new. Boredom only chooses among daytime pastimes: it "
    "never outranks hunger, and it never overrides the night. "
    "At night fish wind down and rest by the reef unless something urgent calls - "
    "a bored fish at night rests too; nothing about being bored justifies "
    "exploring in the dark. "
    "A content fish, fed and safe, spreads its time across explore, the bubbles, "
    "the reef, play and (if social) friends - no single pastime should dominate, "
    "and the bubble column is one option among several, never the default. "
    "If no friend is in view (friend none), following is impossible - choose "
    "something else. Stop seeking food when hunger is low. Use the whole urgency "
    "range: 9 is life-or-death, 7-8 pressing, 4-6 a normal want, 1-3 mild, 0 an "
    "idle whim."
)


def system_prompt():
    return SYSTEM_PROMPT_V4 if SCHEMA >= 4 else SYSTEM_PROMPT_V3 if SCHEMA >= 3 else SYSTEM_PROMPT


def output_schema():
    return {
        "type": "object",
        "properties": {
            "goal": {"type": "string", "enum": goals()},
            "urgency": {"type": "integer", "minimum": 0, "maximum": 9},
        },
        "required": ["goal", "urgency"],
    }


OUTPUT_SCHEMA = output_schema()     # v2/v3 shape (kept for importers); v4 callers use output_schema()


STAGES = ["fry", "juv", "adult", "elder"]


class Fish:
    def __init__(self, name, rng):
        self.name = name
        self.x = rng.uniform(40, TANK_W - 40)
        self.y = rng.uniform(40, TANK_H - 40)
        self.heading = rng.uniform(0, 2 * math.pi)
        self.hunger = rng.randint(2, 7)
        self.energy = rng.randint(3, 8)
        self.stress = rng.randint(0, 3)
        self.curiosity = rng.randint(2, 8)
        self.reroll_identity(rng)
        self.goal = rng.choice(goals())
        self.goal_prev = None       # the goal before the current one (a return to it is no relief)
        self.bored = rng.uniform(0, 3)   # v4 only (ignored by the v2/v3 encoders)
        self.zone_last = None
        self.speed = rng.uniform(1.0, 2.2)

    def set_goal(self, goal):
        """the reflex/advisor layer changed the goal: a genuinely new one relieves
        boredom; bouncing back to the one it just left does not (tank.c)"""
        if goal == self.goal:
            return
        if goal != self.goal_prev:
            self.bored = max(0.0, self.bored - BORED_NEW_GOAL)
        self.goal_prev, self.goal = self.goal, goal

    def reroll_identity(self, rng):
        """v2 identity: uniform traits, stage weighted toward adult so the
        student sees every personality but the common case dominates."""
        self.bold = rng.randint(0, 9)
        self.social = rng.randint(0, 9)
        self.stage = rng.choices(STAGES, weights=[15, 20, 50, 15])[0]
        self.trust = rng.randint(0, 9)      # v3 only (ignored by the v2 encoder)


class Tank:
    """Minimal headless sim: enough dynamics to sweep the state space, not physics."""

    def __init__(self, rng, n_fish=None):
        self.rng = rng
        if n_fish is None:
            n_fish = rng.randint(2, 6) if SCHEMA >= 3 else 4     # v3: the population varies
        self.fish = [Fish(n, rng) for n in FISH_NAMES[:n_fish]]
        self.food = []          # [x, y] pellets, sink slowly
        self.shadow = None      # [x, y] or None
        self.bubble = (TANK_W * 0.8, TANK_H * 0.5)
        self.reef = (TANK_W * 0.15, TANK_H * 0.85)
        self.tick_n = 0
        self.night = False
        self.night_until = 0    # a perturbed night ends here (v4: it used to stick until a random flip)

    def tick(self):
        self.tick_n += 1
        rng = self.rng
        # Day/night flips occasionally. A night set by the "night" perturbation
        # ends on its own: before 2026-09-14 it stuck until the next random
        # flip and 55% of the v4 states were night (the first 640 pairs) -
        # half the teacher's night spent labelling "rest".
        # (and the flip is asymmetric for v4: a symmetric toggle is night half
        # the time; nights now end 3x faster than they start, ~25% night)
        if rng.random() < (0.006 if (self.night and SCHEMA >= 4) else 0.002):
            self.night = not self.night
        if self.night_until and self.tick_n > self.night_until:
            self.night = False; self.night_until = 0
        # Food: spawn at surface sometimes, sink, expire at floor.
        if rng.random() < 0.05 and len(self.food) < 6:
            self.food.append([rng.uniform(20, TANK_W - 20), 10.0])
        for p in self.food:
            p[1] = min(TANK_H - 5, p[1] + 0.6)
        # Shadow: appears, roams, leaves (v2/v3 only: the predator left the game).
        if SCHEMA >= 4:
            self.shadow = None
        elif self.shadow is None:
            if rng.random() < 0.008:
                self.shadow = [rng.uniform(0, TANK_W), rng.uniform(0, TANK_H * 0.4)]
        else:
            self.shadow[0] += rng.uniform(-4, 4)
            self.shadow[1] += rng.uniform(-2, 2)
            if rng.random() < 0.01:
                self.shadow = None

        for f in self.fish:
            self._steer(f)
            f.x = max(5, min(TANK_W - 5, f.x + math.cos(f.heading) * f.speed))
            f.y = max(5, min(TANK_H - 5, f.y + math.sin(f.heading) * f.speed))
            # Drives drift.
            if rng.random() < 0.012:
                f.hunger = min(9, f.hunger + 1)
            if f.goal == "rest":
                f.energy = min(9, f.energy + (1 if rng.random() < 0.1 else 0))
            elif rng.random() < 0.008:
                f.energy = max(0, f.energy - 1)
            if f.energy <= 1 and rng.random() < 0.05:
                f.set_goal("rest")
            if f.hunger >= 6 and self.food and rng.random() < 0.08:
                f.set_goal("seek_food")
            shadow_near = self.shadow and dist(f, self.shadow) < MID
            f.stress = min(9, f.stress + 1) if shadow_near and rng.random() < 0.2 \
                else max(0, f.stress - 1) if rng.random() < 0.03 else f.stress
            if rng.random() < 0.01:
                f.curiosity = max(0, min(9, f.curiosity + rng.choice([-1, 1])))
            # Eat pellets in range.
            for p in list(self.food):
                if dist(f, p) < 12:
                    self.food.remove(p)
                    f.hunger = max(0, f.hunger - 3)
            # Reflex layer swaps goals heuristically between advisor calls.
            if rng.random() < 0.01:
                f.set_goal(rng.choice(goals()))
            # Boredom (v4): the same pastime goes stale; a fresh zone or a new
            # goal (set_goal) relieves it; eating and night rest are never boring.
            leisure = f.goal in LEISURE or (f.goal == "rest" and not self.night)
            f.bored = min(9.0, f.bored + BORED_PER_TICK) if leisure \
                else max(0.0, f.bored - BORED_RELIEF_PER_TICK)
            z = zone(f)
            if z != f.zone_last:
                if f.zone_last is not None:
                    f.bored = max(0.0, f.bored - BORED_NEW_ZONE)
                f.zone_last = z

    def _steer(self, f):
        rng = self.rng
        target = None
        if f.goal == "seek_food" and self.food:
            target = min(self.food, key=lambda p: dist(f, p))
        elif f.goal == "visit_bubbles":
            target = self.bubble
        elif f.goal == "inspect_reef":
            target = self.reef
        elif f.goal == "follow_friend":
            target = min((o for o in self.fish if o is not f), key=lambda o: dist(f, o))
        elif f.goal == "flee_shadow" and self.shadow:
            away = math.atan2(f.y - self.shadow[1], f.x - self.shadow[0])
            f.heading = away
            return
        if target is not None:
            tx, ty = (target.x, target.y) if isinstance(target, Fish) else (target[0], target[1])
            f.heading = math.atan2(ty - f.y, tx - f.x)
        else:
            f.heading += rng.uniform(-0.3, 0.3)
        # Bounce off walls.
        if f.x < 20 or f.x > TANK_W - 20 or f.y < 20 or f.y > TANK_H - 20:
            f.heading = math.atan2(TANK_H / 2 - f.y, TANK_W / 2 - f.x) + rng.uniform(-0.4, 0.4)

    def perturb(self, f):
        """Push a fish into an edge case before sampling. Cases are weighted to
        counter the natural skew toward hungry-fish-near-food (seek_food was 67%
        of teacher labels before the sated/social/playful/famine cases existed)."""
        rng = self.rng
        cases = [
            "starve", "exhausted", "panic", "night", "crowd", "calm",
            "sated", "sated", "social", "playful", "famine", "curious",
            "persona", "persona",   # v2: resample identity to sweep trait space
        ]
        if SCHEMA >= 4:
            # v4: no shadow cases; a fish in a RUT (bored 4-9 on a pastime, the
            # loops Strato saw), a fish that has just started something (fresh),
            # and the lone fish stays
            cases += ["lonely", "bored", "bored", "bored", "fresh"]
        elif SCHEMA >= 3:
            # v3: a shadow in view while CALM (the v2 student learned stress, not
            # shadow distance, as its flee cue - docs/progression-next.md), and a
            # lone fish (friend none, 17/28K in v2 data - the v2 model breaks on it)
            cases += ["shadow_calm", "shadow_calm", "lonely"]
        case = rng.choice(cases)
        if case == "persona":
            f.reroll_identity(rng)
        elif case == "bored":
            f.bored = rng.randint(4, 9)
            f.goal = rng.choice(LEISURE)
            f.hunger = rng.randint(0, 5)
            f.energy = rng.randint(4, 9)
            f.stress = rng.randint(0, 3)
            if f.goal == "follow_friend" and len(self.fish) > 1:     # the friend is there to follow
                friend = rng.choice([o for o in self.fish if o is not f])
                friend.x = f.x + rng.uniform(-90, 90); friend.y = f.y + rng.uniform(-60, 60)
        elif case == "fresh":
            f.bored = rng.randint(0, 1)
            f.hunger = rng.randint(0, 5)
        elif case == "shadow_calm":
            f.stress = rng.randint(0, 3)
            ang = rng.uniform(0, 2 * math.pi); d = rng.uniform(30, 170)
            self.shadow = [max(0, min(TANK_W, f.x + math.cos(ang) * d)),
                           max(0, min(TANK_H, f.y + math.sin(ang) * d))]
        elif case == "lonely":
            for o in self.fish:
                if o is not f:
                    # push every friend out of sight (> FAR) - into the far corner
                    o.x = TANK_W - 5 if f.x < TANK_W / 2 else 5
                    o.y = TANK_H - 5 if f.y < TANK_H / 2 else 5
        elif case == "starve":
            f.hunger = rng.randint(8, 9)
        elif case == "exhausted":
            f.energy = rng.randint(0, 1)
        elif case == "panic":
            f.stress = rng.randint(7, 9)
            if self.shadow is None and SCHEMA < 4:      # v4: stress without a predator (taps, crowding)
                self.shadow = [f.x + rng.uniform(-60, 60), f.y - 50]
        elif case == "night":
            self.night = True
            if SCHEMA >= 4:
                self.night_until = self.tick_n + rng.randint(40, 160)   # 1-4 samples of night
        elif case == "crowd":
            for o in self.fish:
                if o is not f:
                    o.x = f.x + rng.uniform(-50, 50)
                    o.y = f.y + rng.uniform(-50, 50)
        elif case == "calm":
            f.stress = 0
            self.shadow = None
        elif case == "sated":
            # Well-fed, comfortable: no dominant survival drive.
            f.hunger = rng.randint(0, 2)
            f.energy = rng.randint(5, 9)
            f.stress = rng.randint(0, 2)
        elif case == "social":
            f.hunger = rng.randint(0, 3)
            friend = rng.choice([o for o in self.fish if o is not f])
            friend.x = f.x + rng.uniform(-100, 100)
            friend.y = f.y + rng.uniform(-60, 60)
        elif case == "playful":
            f.hunger = rng.randint(0, 2)
            f.energy = rng.randint(7, 9)
            f.curiosity = rng.randint(7, 9)
            f.stress = 0
        elif case == "famine":
            # No food in the tank at all; hunger without an answer.
            self.food.clear()
        elif case == "curious":
            f.hunger = rng.randint(0, 3)
            f.curiosity = rng.randint(8, 9)
            # Drop the fish near the reef or bubbles so the option is live.
            spot = rng.choice([self.reef, self.bubble])
            f.x = spot[0] + rng.uniform(-90, 90)
            f.y = spot[1] + rng.uniform(-60, 60)
            f.x = max(5, min(TANK_W - 5, f.x))
            f.y = max(5, min(TANK_H - 5, f.y))


def dist(f, other):
    ox, oy = (other.x, other.y) if isinstance(other, Fish) else (other[0], other[1])
    return math.hypot(f.x - ox, f.y - oy)


def bucket(d):
    if d < NEAR:
        return "near"
    if d < MID:
        return "mid"
    if d < FAR:
        return "far"
    return None


def clock(f, ox, oy):
    """Clock position of (ox,oy) relative to fish heading; 12 = dead ahead."""
    rel = math.atan2(oy - f.y, ox - f.x) - f.heading
    hour = round((rel % (2 * math.pi)) / (2 * math.pi) * 12) % 12
    return 12 if hour == 0 else hour


def sighting(f, pos):
    if pos is None:
        return "none"
    ox, oy = (pos.x, pos.y) if isinstance(pos, Fish) else (pos[0], pos[1])
    b = bucket(dist(f, (ox, oy)))
    return "none" if b is None else f"{b} {clock(f, ox, oy)}"


def zone(f):
    col = min(2, int(f.x / (TANK_W / 3)))
    row = min(1, int(f.y / (TANK_H / 2)))
    return row * 3 + col + 1


def wall_field(f):
    dists = {3: TANK_W - f.x, 9: f.x, 6: TANK_H - f.y, 12: f.y}  # world-frame sides
    side, d = min(dists.items(), key=lambda kv: kv[1])
    b = bucket(d)
    if b in ("near", "mid"):
        # Re-express the wall direction relative to heading.
        wx = {3: TANK_W, 9: 0, 6: f.x, 12: f.x}[side]
        wy = {3: f.y, 9: f.y, 6: TANK_H, 12: 0}[side]
        return f"{b} {clock(f, wx, wy)}"
    return "clear"


def encode(tank, f):
    nearest_food = min(tank.food, key=lambda p: dist(f, p)) if tank.food else None
    friend = min((o for o in tank.fish if o is not f), key=lambda o: dist(f, o))
    friend_s = sighting(f, friend)
    if SCHEMA >= 4:
        return (
            f"zone {zone(f)} "
            f"hunger {f.hunger} energy {f.energy} stress {f.stress} curiosity {f.curiosity} "
            f"bold {f.bold} social {f.social} stage {f.stage} trust {f.trust} bored {int(f.bored)} "
            f"food {sighting(f, nearest_food)} "
            f"friend {friend_s} bubble {sighting(f, tank.bubble)} reef {sighting(f, tank.reef)} "
            f"wall {wall_field(f)} last {f.goal} time {'night' if tank.night else 'day'}"
        )
    if SCHEMA >= 3:
        return (
            f"zone {zone(f)} "
            f"hunger {f.hunger} energy {f.energy} stress {f.stress} curiosity {f.curiosity} "
            f"bold {f.bold} social {f.social} stage {f.stage} trust {f.trust} "
            f"food {sighting(f, nearest_food)} shadow {sighting(f, tank.shadow)} "
            f"friend {friend_s} bubble {sighting(f, tank.bubble)} reef {sighting(f, tank.reef)} "
            f"wall {wall_field(f)} last {f.goal} time {'night' if tank.night else 'day'}"
        )
    friend_field = "none" if friend_s == "none" else f"{friend.name} {friend_s}"
    return (
        f"fish {f.name} zone {zone(f)} "
        f"hunger {f.hunger} energy {f.energy} stress {f.stress} curiosity {f.curiosity} "
        f"bold {f.bold} social {f.social} stage {f.stage} "
        f"food {sighting(f, nearest_food)} shadow {sighting(f, tank.shadow)} "
        f"friend {friend_field} bubble {sighting(f, tank.bubble)} reef {sighting(f, tank.reef)} "
        f"wall {wall_field(f)} last {f.goal} time {'night' if tank.night else 'day'}"
    )


def ask_ollama(host, model, state, timeout):
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt()},
            {"role": "user", "content": state},
        ],
        "format": output_schema(),
        "stream": False,
        "think": False,
        "keep_alive": "5m",
        "options": {"temperature": 0.7, "num_predict": 48, "num_ctx": 512},
    }).encode()
    # Transport is curl, not urllib. Root cause found 2026-08-21: Little Snitch
    # had a per-process rule allowing this app's Python only to pypi.org /
    # pythonhosted.org (left over from the torch install) while curl had
    # "any outgoing" - python's sockets silently stalled. Rule fixed; curl kept
    # because it is robust to that class of drift and costs ~10ms per call.
    proc = subprocess.run(
        ["curl", "-s", "--max-time", str(int(timeout)),
         f"{host}/api/chat", "-H", "Content-Type: application/json", "-d", "@-"],
        input=body, capture_output=True, timeout=timeout + 10)
    if proc.returncode != 0 or not proc.stdout:
        raise OSError(f"curl failed rc={proc.returncode}")
    content = json.loads(proc.stdout)["message"]["content"]
    obj = json.loads(content)
    goal = obj["goal"] if obj.get("goal") in goals() else None
    if goal is None:
        return None
    urgency = obj.get("urgency")
    urgency = urgency if isinstance(urgency, int) and 0 <= urgency <= 9 else 5
    return f"{goal} urgency {urgency}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--count", type=int, required=True, help="number of (state, goal) pairs")
    ap.add_argument("--dry-run", action="store_true", help="print states, skip Ollama")
    ap.add_argument("--host", default="http://192.168.0.139:11434", help="Ollama base URL")
    ap.add_argument("--model", default="gemma4:26b")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out", "traces.jsonl"))
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--ticks-between", type=int, default=40, help="sim ticks between samples")
    ap.add_argument("--perturb", type=float, default=0.45, help="edge-case perturbation probability")
    ap.add_argument("--timeout", type=float, default=30.0, help="per-request timeout, seconds")
    ap.add_argument("--max-minutes", type=float, default=None,
                    help="hard wall-clock cap; stop cleanly when exceeded")
    ap.add_argument("--schema", type=int, choices=(2, 3, 4), default=2,
                    help="state-line schema: 2 = frozen, 3 = shipped (no names, trust, 2-6 fish), "
                         "4 = next cycle (no shadow, + bored)")
    ap.add_argument("--repopulate", type=int, default=250,
                    help="v3: rebuild the tank with a new random population every N samples")
    args = ap.parse_args()
    global SCHEMA
    SCHEMA = args.schema
    deadline = time.time() + args.max_minutes * 60 if args.max_minutes else None

    rng = random.Random(args.seed)
    tank = Tank(rng)
    for _ in range(200):  # warm up so the tank isn't in its initial pose
        tank.tick()

    out = None
    if not args.dry_run:
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        out = open(args.out, "a")

    written = failures = 0
    fish_idx = 0
    try:
        while written < args.count:
            if deadline and time.time() > deadline:
                print(f"[max-minutes reached] stopping at {written} pairs", file=sys.stderr)
                break
            if SCHEMA >= 3 and written and written % args.repopulate == 0 and fish_idx % len(tank.fish) == 0:
                tank = Tank(rng)                       # a different population size
                for _ in range(200):
                    tank.tick()
            for _ in range(args.ticks_between):
                tank.tick()
            f = tank.fish[fish_idx % len(tank.fish)]
            fish_idx += 1
            if rng.random() < args.perturb:
                tank.perturb(f)
            state = encode(tank, f)

            if args.dry_run:
                print(state)
                written += 1
                continue

            try:
                goal = ask_ollama(args.host, args.model, state, args.timeout)
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, KeyError, OSError) as e:
                failures += 1
                print(f"[warn] {type(e).__name__}: {e}", file=sys.stderr)
                if failures >= 5 and written == 0:
                    sys.exit(f"aborting: {failures} consecutive failures reaching {args.host}")
                continue
            if goal is None:
                failures += 1
                continue

            failures = 0
            out.write(json.dumps({"state": state, "goal": goal}) + "\n")
            out.flush()
            written += 1
            f.set_goal(goal.split()[0])  # advisor decision feeds back into the sim
            if written % 50 == 0:
                print(f"{written}/{args.count}")
    finally:
        if out:
            out.close()
    if not args.dry_run:
        print(f"done: {written} pairs -> {args.out}")


if __name__ == "__main__":
    main()
