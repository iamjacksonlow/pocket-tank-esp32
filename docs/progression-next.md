# Progression II — proposal (2026-08-21, unbuilt)

Answers the open problem in HANDOFF.md. Supersedes nothing in progression.md
(the contract still holds); this is the plan for what the progression layer
*is*, now that breeding-as-population is out. Every number here is reproducible
with `model/probe_dist.py` (CPU, shipped `ckpt_v2w.pt`, ~1 min).

## The question, restated

"Population as progression, capped at 4" was the candidate direction. Strato's
brief for this pass: don't assume fewer fish is the answer; think about the
whole player experience, the sense of progression, and what the LLM
architecture gives that rules can't — and validate that advantage (efficiency,
randomization, UX).

## Evidence gathered (changes the recommendation)

### 1. A one-fish tank is not an option — the model breaks, it doesn't degrade

`friend none` appears in **17 of 28,312** training pairs (0.06%). Probed:

| state (otherwise identical) | friend bolt mid 3 | friend none |
|---|---|---|
| starving, food near 12 | seek_food 0.99 | **flee_shadow 0.97** (no shadow exists) |
| shadow near 12 | explore 0.62 / flee 0.19 | **visit_bubbles 0.62** |
| content | inspect_reef 0.58 | visit_bubbles 0.46 / seek_food 0.25 (not hungry) |

A lone fish would be permanently out of distribution. Two fish is the floor
(the `friend` field is then always populated; nothing else in the state line
encodes population). Starting with 1 would need a data cycle — not worth it.

### 2. The model's goal *distribution* is real signal, and we throw it away

The engine decodes greedily (argmax). Probing the softmax over the 8 goal
tokens on 800 real dataset states:

| measure | value |
|---|---|
| teacher's label == student's top-1 | 79% |
| teacher's label in student's **top-2** | **94%** |
| states where top-2 margin < 0.2 ("torn") | 14% |
| mean top-1 probability | 0.80 (46% of states > 0.9; 21% < 0.6) |
| P(sample ≠ greedy) at T = 1 | 20% |
| starving (hunger 9, food near), min P(seek_food) over 18 identities | **0.89** (adult 0.99; timid stressed fry 0.97) |
| hunger 4..8 with food near, P(seek_food) | 0.95 → 1.00 |

So: sampling from the model's own distribution would change ~1 in 5 decisions,
almost always to the teacher's *own* second choice, while survival decisions
stay essentially untouched. Greedy decoding collapsed the teacher's variety
(the teacher ran at T = 0.7; its self-agreement is 82%); the student learned
that variety as soft probability and we currently discard it.

### 3. Identity conditioning is real — and it has *cliffs*

Content fish (hunger 2, energy 7, stress 0, curiosity 7, nothing urgent):

| axis | what happens |
|---|---|
| bold 0 → 7 | smooth: inspect_reef 0.67 → 0.51, visit_bubbles 0.18 → 0.32 (per-step TV distance 0.02–0.09) |
| bold 8, 9 | **regime change**: visit_bubbles overtakes reef; **dart_play appears** 0.09 → 0.25 (TV 0.19, 0.23) |
| social 0, 3, 6 | follow_friend 0.01–0.04 |
| social 9 | **follow_friend 0.85** — a cliff between 6 and 9 |
| stage fry | follow_friend 0.36, entropy **1.50** (erratic) |
| stage juv | dart_play 0.12, entropy 1.39 |
| stage adult / elder | entropy 1.10 / 1.17 (settled) |
| night, juv vs elder | juv rest 0.18 (bubbles 0.52); **elder rest 0.97** |
| names mira/bolt/kelp/nori | identical distributions (names carry zero signal → arrivals can take any name) |

This is the whole case for the LLM in one table: the teacher prompt said
"bold 7-9 play … only a very social fish (7-9) follows friends … fry stay near
friends … elder rest more", and the student turned that prose into a joint
function over identity × situation with **no code**. A trait drifting across
a cliff (social 6 → 7+, bold 7 → 8+) is a behavior *unlock* the player can
see. Fry are more unpredictable *because the model is less certain about
them* — no per-fish temperature hack needed.

### 4. Two side findings

- The student learned **stress** as its flee cue, not shadow distance:
  `shadow near 12` at stress 1 → flee 0.19 (dataset: 98% flee), at stress 8 →
  0.82; timid stressed fry → 0.95. In the data 80% of shadow-near states have
  stress ≥ 7 because the sim ramps stress near shadows, and the tank does the
  same (+1.7/s inside 115 px), so practice is fine. Note for the next regen:
  perturb stress low with shadow near.
- **The firmware has no player feed gesture** (sim: F key; tank.c auto-trickles
  a pellet when < 2 are live). Care-based progression needs one.

## Thesis

**Progression = the fish becoming characters, and the LLM is the only part of
the system that turns a number into character.** Rules can move `bold` from 4
to 8; only the model makes `bold 8` mean "dart_play appears, bubbles over reef,
shrugs off a mid shadow" in every situation at once. So progression should
(a) push the numbers the model reads, (b) let the player see the model read
them, and (c) use the model's distribution for variety and visible deliberation.
Population growth is the *opening act*, not the system.

What the LLM gives that `advisor.c` (44 lines of rules) cannot — the claims to
validate for the video:

| LLM advantage | evidence / mechanism | rules equivalent |
|---|---|---|
| Identity as continuous conditioning: 400 identity cells × every situation | bold/social/stage sweeps above | thresholds (`bold < 0.35`, `sociable > 0.62`), one branch per case |
| Situated randomness: varies only among *plausible* choices, weighted by judgment | teacher label in top-2 94%; starving min 0.89 | `rand()` — uniform noise, or hand-weighted tables per situation |
| Confidence: knows how close the second choice was | 14% torn states | no notion of a runner-up |
| Behavior spec is prose → data → weights | "prompt-is-DNA": one sentence moved follow_friend 45% → 14% | edit C, retune constants |
| Believability: the decisions are the teacher's judgment incl. its "mistakes" that read as personality | stats.md misses: timid fish fled instead of eating; fry stayed with friends | scripted exceptions |

## The design: three acts and a long tail

### Act 1 — the pair (day 1)

Ship with **two adults**. Recommend **bolt + kelp**: the most opposite pair
(bold 0.90 / social 0.40 vs bold 0.18 / social 0.88) so the contrast is
readable in a glance — dart bursts vs reef-hugging and friend-following.
Mira and nori arrive later (names carry no signal, so this is pure story).
Bonus: shared-advisor cadence is 2× faster with two fish (4.6 s vs 9 s at
QEMU speed) exactly when the player is learning what fish do.

### Act 2 — arrivals, and raising them (days 1–7)

The 3rd and 4th fish arrive as **fry**, unannounced (light-on in the morning,
or after a night: "there's something in the reef"). This is where the stage
system finally earns its keep — the shipped tank starts adult, so fry/juv
behavior only ever shows on arrivals — and the model renders "fry" for free
(follow 0.36, erratic, reef-and-friends).

- Triggers are **care, never time alone** (contract: shaped by attention):
  - 3rd: both adults trust ≥ 6 **and** ~N player feedings **and** one calm
    hold-approach observed.
  - 4th: the 3rd reached juv **and** at least one adult's bold or social has
    drifted ≥ 1 unit from its shipped value (the player has changed someone).
- **Inheritance** (small, LLM-adjacent trick): the fry's bold/social = mean of
  the two adults ± noise. The model renders the child's mix with no new code.
  "Breeding" is the *story* of an arrival (two thriving adults), never a 5th.
- Stage thresholds are tuned for one afternoon today (20 min / 90 min / 8 h).
  For a desk companion glanced at over weeks, propose ~1 h / 6 h / 48 h tended
  (Strato's call; pure constants).

### Act 3 — character (weeks)

Trait drift is the progression that never ends, and now it has **unlocks**:
social crossing 6 → 7+ (a fish that starts following friends), bold crossing
7 → 8+ (dart_play appears). Make drift rates feel-able — today one unit per
~6 h of pressure is invisible; propose ~2 h, tuned by watching the sim — and
keep the shipped value as a soft floor so absence plateaus and nothing is
ruined. Elders settle (rest 0.97 at night, lower entropy): the old fish
becomes the one you *know*.

### Long tail

- **Sampling** keeps evenings from repeating (20% of decisions vary, all
  plausible).
- Rare shadow events play out differently per identity (fry 0.95 flee vs bold
  adult 0.28 at the same stress).
- Milestones (below) keep a collection going without cluttering the tank.

## The "see them think" layer (new, LLM-only, ~free)

1. `goal_t` gains `float confidence` (top-1 prob, or top-1 − top-2 margin).
   `q4_model` exposes the goal-token softmax from the prefill logits it already
   computes (8 exps; no extra weight pass).
2. **Sampling** replaces argmax for the goal token, using the tank's
   deterministic xorshift so the sim stays reproducible. T = 1 — the model's
   own distribution, per [[llm-owns-decisions]]: nothing overrides it, and the
   stage-dependent unpredictability is already in the weights.
3. **Hesitation**: when switching goals with low confidence, the reflex layer
   inserts a short pause and a glance toward the runner-up's target (food vs
   friend) before committing; high confidence commits instantly. 14% of states
   are torn → visible a few times a minute across the tank. A rule engine has
   no runner-up to glance at.
4. Optional, stat card: the runner-up goal icon at partial opacity ("on her
   mind") — visual only, fits the no-digits rule.

Guardrail: measure, don't patch — run the 10-min sim soak before/after and
report `tank_reflex_overrides` (starving-ignored episodes) and goal variety.
Expectation from the probe: survival unchanged (P(seek) ≥ 0.95 at hunger ≥ 4
with food near), variety up.

## Milestones — detected, not scripted

Separate view (contract). Per fish: arrival; first meal from you; first
hold-approach; first dart_play; first bubble visit; first shadow survived
(fled, then came back); first *shrug* (shadow mid in view, chose not to flee);
first follow (the social unlock); juv / adult / elder reached. Tank: pair →
trio → quartet; first night everyone resting at the reef; "the tank changed
someone" (a trait crossed a regime threshold).

The LLM angle: the behavior milestones are the model's own choices under the
fish's drifted identity, so *when* Kelp first shrugs off a shadow is earned
and differs per tank. A scripted tank would have to stage it.

## What to build, in order

1. Distribution: goal softmax + `confidence` out of `common/llm`, sampling in
   `advisor_llm` (sim + esp), hesitation in `tank.c`. Soak + compare.
2. Population ≤ 4: `active` mask in `tank_t`; friend/render/advisor/persist
   honor it; 2-fish boot; arrival triggers + fry inheritance; stage threshold
   constants.
3. Milestones store (NVS, a few dozen bits + timestamps) and view.
4. Device feed gesture (e.g. swipe down / tap at the surface → pellets at x).
5. Later data cycle (optional): stress-low shadow perturbation; schema v3
   (drop names, maybe add trust) only if ever needed.

Performance: all of (1) is free (logits exist); (2) only ever lowers load.

## Open for Strato

- Starting pair: bolt + kelp (max contrast) vs mira + someone (protagonist
  story)?
- Stage thresholds and drift rate for a weeks-long desk companion.
- Sampling T = 1 (pure model) vs slightly cooler (0.8) — the probe says T = 1
  is safe; it's a feel question.
- Arrival presentation: morning reveal at the reef, or any light-on?

## Decisions 2026-08-21 (Strato, after review)

- Agreed with the proposal. **Starting pair is random** per new tank (not a
  fixed bolt+kelp). Recommend rolling personalities with a guaranteed contrast
  (e.g. |bold_a − bold_b| ≥ 4) so Act 1 is still readable.
- **Population cap raised above 4** (5 or 6; ceiling is comfortable because the
  tank starts at 2 — 3–4 fry cycles is a lot of play). Names carry zero signal
  (probe: identical distributions), so fish 5–6 reuse a trained name token in
  the state line as a placeholder (display name is free); schema v3 "drop
  names" becomes a cleanup, not a prerequisite. Cadence: move from round-robin
  to the browser prototype's need-based scheduler (coarse state signature
  changed / urgent / idle ceiling) so effective cadence scales with activity,
  not headcount. Recommendation: `N_FISH_MAX 6` compile-time, ship 5 until
  hardware latency is measured.


## Status 2026-08-21 (evening): foundation BUILT

Everything in "What to build, in order" 1–4 landed this session (see
docs/HANDOFF.md for the file-level map); 5 (the v3 data cycle) is prepared in
docs/retrain-v3.md and waits for Strato's go. Measured after the build:
`./fishsim --selftest-llm` (4 fish, 60 sim-s, real model, sampled): 54 goal
changes, 19 of them "torn" (p < 0.6 → visible hesitation), 113 need-based
asks, 2 starving-ignored episodes. Snapshots of the renderer (elder crest,
earned stripes, fry size, stats card with reveal-gated bars, milestones view)
were checked visually. Firmware rebuilt clean (383 KB app, POP_CAP 5).
