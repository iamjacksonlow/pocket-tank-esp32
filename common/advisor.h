/* advisor.h — deliberative layer interface.
 *
 * The tank calls a plugged-in advisor_fn (see tank.h) on a slow cadence.
 * advisor_rules is the reflex-only stub ported from the prototype's
 * chooseCompanionGoal(): personality-driven, no model. The firmware swaps in
 * an LLM-backed advisor with the same signature (Track 3). */
#ifndef ADVISOR_H
#define ADVISOR_H

#include "tank.h"

goal_t advisor_rules(const tank_t *t, int fish_idx, bool request);

/* LLM advisor (advisor_llm.c): trained llama2.c student, async worker thread.
 * init returns false if model files are missing; advisor_llm then no-ops.
 * Decisions submitted on request=true land on the first poll after inference
 * finishes (~tens of ms), never a full cadence later. */
bool advisor_llm_init(const char *model_path, const char *tok_path);
goal_t advisor_llm(const tank_t *t, int fish_idx, bool request);
void advisor_llm_debug(const tank_t *t, int fish_idx);
extern bool advisor_llm_narrate;   /* stream "fish sees / decides" to stdout */

#endif
