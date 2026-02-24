#include "ai.h"
#include "combat.h"
#include "physics.h"
#include <math.h>
#include <string.h>
#include <float.h>

// ---------------------------------------------------------------------------
// RNG
// ---------------------------------------------------------------------------
static uint32_t rng_next(uint32_t *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}
static float rng_f(uint32_t *s) {
    return (float)(rng_next(s) & 0xFFFFFF) / (float)0x1000000;
}
static int rng_i(uint32_t *s, int n) {
    return (int)(rng_f(s) * (float)n);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void ai_init(AIBrain *ai) {
    memset(ai, 0, sizeof(*ai));
    ai->rng               = 0xDEADBEEF;
    ai->aggression        = 60.0f;
    ai->aggression_timer  = 180;
    ai->reaction_delay    = 6;
    ai->preferred_dist    = 120.0f;
    ai->decision_cooldown = 20;
    ai->avg_attack_interval = 60;
    ai->current_pattern   = PAT_PRESSURE;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool ai_can_act(const Player *p) {
    return p->state != STATE_STUNNED &&
           p->state != STATE_ATTACK  &&
           p->state != STATE_PARRY   &&
           p->state != STATE_THROW   &&
           p->state != STATE_DEAD;
}

static float ai_attack_range(void) {
    return SWORD_LENGTH + PLAYER_W * 0.5f + 10.0f;
}

// ---------------------------------------------------------------------------
// Perception
// ---------------------------------------------------------------------------
static AIPerception ai_perceive(const Player *ai_p,
                                const Player *human,
                                const ThrowingSword *swords, int num_swords) {
    AIPerception p;
    memset(&p, 0, sizeof(p));

    p.dist           = (human->body.pos.x + PLAYER_W*0.5f) - (ai_p->body.pos.x + PLAYER_W*0.5f);
    p.abs_dist       = fabsf(p.dist);
    p.human_vel_x    = human->body.vel.x;
    p.human_attacking= (human->state == STATE_ATTACK);
    p.human_parrying = (human->state == STATE_PARRY);
    p.human_has_sword= human->has_sword;

    p.ground_sword_x = FLT_MAX;
    p.sword_x        = FLT_MAX;
    float best_air = FLT_MAX, best_gnd = FLT_MAX;

    for (int i = 0; i < num_swords; i++) {
        const ThrowingSword *s = &swords[i];
        if (!s->active) continue;
        float sd = fabsf(s->pos.x - (ai_p->body.pos.x + PLAYER_W*0.5f));
        bool grounded = (s->pos.y >= g_ground_y() - 2.0f) &&
                        fabsf(s->vel.x) < 5.0f && fabsf(s->vel.y) < 5.0f;
        if (grounded) {
            if (sd < best_gnd) {
                best_gnd = sd;
                p.ground_sword_x = s->pos.x;
                p.ground_sword_y = s->pos.y;
                p.ground_sword_exists = true;
            }
        } else {
            if (sd < best_air) {
                best_air = sd;
                p.sword_x = s->pos.x;
                p.sword_y = s->pos.y;
                p.sword_vel_x = s->vel.x;
                p.sword_owner = s->owner;
                p.sword_in_air = true;
                p.sword_rebounding = s->rebounding;
            }
        }
    }
    return p;
}

// ---------------------------------------------------------------------------
// Movement helpers
// ---------------------------------------------------------------------------
static void ai_move_toward(AIBrain *ai, float ai_cx, float target_x, float tolerance) {
    float d = target_x - ai_cx;
    ai->hold_left  = false;
    ai->hold_right = false;
    if      (d >  tolerance) ai->hold_right = true;
    else if (d < -tolerance) ai->hold_left  = true;
}

static void ai_move_away(AIBrain *ai, float ai_cx, float human_cx) {
    ai->hold_left  = (human_cx > ai_cx);
    ai->hold_right = (human_cx < ai_cx);
}

// ---------------------------------------------------------------------------
// Platform / vertical follow helper
// The AI needs to jump to reach a target that is above it on a platform.
// Called by movement patterns whenever the AI is trying to reach a target X.
// Returns true if a jump was queued this call.
// ---------------------------------------------------------------------------
static bool ai_follow_vertical(AIBrain *ai, const Player *ai_p, float target_y) {
    // target_y is the top of the target's body (or the sword's y)
    float ai_foot = ai_p->body.pos.y + ai_p->body.size.y;
    float gap = ai_foot - target_y;  // positive means target is above AI feet

    // Target is meaningfully above us (more than half a player height)
    if (gap > PLAYER_HEIGHT * 0.5f && ai_p->body.on_ground && ai->jump_in == 0) {
        ai->jump_in = 2;
        return true;
    }
    return false;
}

// Move toward target X and follow vertically if needed.
// This replaces raw ai_move_toward calls in all patterns that chase the human.
static void ai_chase(AIBrain *ai, const Player *ai_p,
                     float ai_cx, float target_cx, float target_y,
                     float tolerance) {
    ai_move_toward(ai, ai_cx, target_cx, tolerance);
    ai_follow_vertical(ai, ai_p, target_y);
}

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------
static void ai_queue(AIBrain *ai, const AIPattern *pats, int n) {
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) ai->pattern_queue[i] = pats[i];
    ai->queue_len  = n;
    ai->queue_head = 0;
}

static void ai_remember_pattern(AIBrain *ai, AIPattern pat) {
    ai->last_patterns[ai->last_pattern_idx % 3] = pat;
    ai->last_pattern_idx++;
}

static bool ai_pattern_recent(const AIBrain *ai, AIPattern pat) {
    for (int i = 0; i < 3; i++)
        if (ai->last_patterns[i] == pat) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------
static void ai_tick_timers(AIBrain *ai) {
    if (ai->jump_in        > 0) ai->jump_in--;
    if (ai->attack_in      > 0) ai->attack_in--;
    if (ai->parry_in       > 0) ai->parry_in--;
    if (ai->throw_in       > 0) ai->throw_in--;
    if (ai->feint_timer    > 0) ai->feint_timer--;
    if (ai->catch_timer    > 0) ai->catch_timer--;
    if (ai->pattern_timer  > 0) ai->pattern_timer--;
    if (ai->decision_cooldown > 0) ai->decision_cooldown--;
    if (ai->aggression_timer  > 0) ai->aggression_timer--;
    if (ai->react_timer       > 0) ai->react_timer--;
}

// ---------------------------------------------------------------------------
// Build output
// ---------------------------------------------------------------------------
static void ai_build_input(AIBrain *ai, Input *out, uint32_t frame) {
    memset(out, 0, sizeof(*out));
    out->frame        = frame;
    out->left         = ai->hold_left;
    out->right        = ai->hold_right;
    out->crouch       = ai->hold_crouch;
    out->parry        = ai->hold_parry;
    out->jump         = (ai->jump_in    == 1);
    out->attack       = (ai->attack_in  == 1);
    out->throw_weapon = (ai->throw_in   == 1);
    if (ai->parry_in == 1) out->parry = true;
}

// ---------------------------------------------------------------------------
// Threat detection
// ---------------------------------------------------------------------------
static bool ai_sword_incoming(const AIBrain *ai, const Player *ai_p,
                               const AIPerception *perc) {
    (void)ai;
    if (!perc->sword_in_air) return false;
    float ai_cx = ai_p->body.pos.x + PLAYER_W*0.5f;
    float to_ai = ai_cx - perc->sword_x;
    return (to_ai * perc->sword_vel_x > 0.0f) &&
           (fabsf(perc->sword_x - ai_cx) < 400.0f);
}

static bool ai_should_catch(const AIBrain *ai, const Player *ai_p,
                             const AIPerception *perc) {
    (void)ai;
    if (!ai_p->has_sword && perc->sword_in_air && perc->sword_rebounding) {
        float ai_cx = ai_p->body.pos.x + PLAYER_W*0.5f;
        float to_ai = ai_cx - perc->sword_x;
        return (to_ai * perc->sword_vel_x > 0.0f) &&
               fabsf(perc->sword_x - ai_cx) < 350.0f;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Decision maker
// ---------------------------------------------------------------------------
static AIPattern ai_choose_pattern(AIBrain *ai, const Player *ai_p,
                                   const Player *human,
                                   const AIPerception *perc) {
    (void)human;
    float agg = ai->aggression;

    // --- Unarmed logic ---
    if (!ai_p->has_sword) {
        if (ai_should_catch(ai, ai_p, perc))  return PAT_CATCH_WAIT;
        // Sword exists somewhere — always go for retrieval
        if (perc->ground_sword_exists || perc->sword_in_air)
            return PAT_SWORD_RETRIEVE;
        // No sword at all — keep distance
        return perc->abs_dist < 200.0f ? PAT_RETREAT_UNARMED : PAT_RETREAT_UNARMED;
    }

    // --- Armed logic ---
    if (perc->human_attacking && perc->abs_dist < ai_attack_range() + 30.0f)
        return PAT_BAIT_PARRY;
    if (ai_sword_incoming(ai, ai_p, perc))
        return PAT_BAIT_PARRY;

    float roll = rng_f(&ai->rng) * 100.0f;

    if (perc->abs_dist > 350.0f) {
        if (roll < agg * 0.4f) return PAT_RUSH_ATTACK;
        if (roll < agg * 0.6f && !ai_pattern_recent(ai, PAT_RETREAT_THROW))
            return PAT_RETREAT_THROW;
        return PAT_PRESSURE;
    }
    if (perc->abs_dist > 180.0f) {
        if (roll < 20.0f && !ai_pattern_recent(ai, PAT_JUMP_ATTACK))   return PAT_JUMP_ATTACK;
        if (roll < 40.0f && !ai_pattern_recent(ai, PAT_FEINT_ATTACK))  return PAT_FEINT_ATTACK;
        if (roll < 55.0f) return PAT_PRESSURE;
        if (roll < 70.0f && !ai_pattern_recent(ai, PAT_SPACING_DANCE)) return PAT_SPACING_DANCE;
        if (roll < 80.0f && !ai_pattern_recent(ai, PAT_RETREAT_THROW)) return PAT_RETREAT_THROW;
        return PAT_BAIT_PARRY;
    }
    // Close range
    if (roll < 25.0f && !ai_pattern_recent(ai, PAT_CROUCH_POKE))   return PAT_CROUCH_POKE;
    if (roll < 45.0f) return PAT_PRESSURE;
    if (roll < 60.0f) return PAT_BAIT_PARRY;
    if (roll < 75.0f && !ai_pattern_recent(ai, PAT_JUMP_ATTACK))   return PAT_JUMP_ATTACK;
    if (roll < 85.0f && !ai_pattern_recent(ai, PAT_FEINT_ATTACK))  return PAT_FEINT_ATTACK;
    return PAT_THROW_FOLLOWUP;
}

// ---------------------------------------------------------------------------
// Pattern executors
// ---------------------------------------------------------------------------

static bool exec_pressure(AIBrain *ai, const Player *ai_p,
                          const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (perc->abs_dist > ai_attack_range() - 10.0f) {
        ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
        ai->pattern_timer = 60;
        return false;
    }
    if (ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 2 + rng_i(&ai->rng, 3);
        return true;
    }
    return false;
}

static bool exec_feint(AIBrain *ai, const Player *ai_p,
                       const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    float feint_range = ai_attack_range() + 30.0f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        if (perc->abs_dist > feint_range) {
            ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
            return false;
        }
        ai->feinting    = true;
        ai->feint_timer = 8 + rng_i(&ai->rng, 8);
        ai->pattern_phase = 1;
        return false;
    }
    if (ai->pattern_phase == 1) {
        if (ai->feint_timer > 0) return false;
        ai->pattern_phase = 2;
        return false;
    }
    // Phase 2: burst in and attack
    if (perc->abs_dist > ai_attack_range() - 5.0f) {
        ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
        return false;
    }
    if (ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

static bool exec_jump_attack(AIBrain *ai, const Player *ai_p,
                              const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        if (ai_p->body.on_ground && ai->jump_in == 0) {
            ai->jump_in       = 2;
            ai->pattern_phase = 1;
        }
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        return false;
    }
    // In air — track horizontally AND try to match target height
    ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
    if (perc->abs_dist < ai_attack_range() + 20.0f && ai->attack_in == 0) {
        ai->attack_in = 2;
        return true;
    }
    if (ai_p->body.on_ground && ai->pattern_phase == 1) return true;
    return false;
}

static bool exec_crouch_poke(AIBrain *ai, const Player *ai_p,
                              const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    float tight_range = ai_attack_range() - 15.0f;
    if (perc->abs_dist > tight_range) {
        ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
        return false;
    }
    if (ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

static bool exec_retreat_throw(AIBrain *ai, const Player *ai_p,
                                const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float retreat_dist = 250.0f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        if (perc->abs_dist < retreat_dist) {
            ai_move_away(ai, ai_cx, human_cx);
            return false;
        }
        ai->pattern_phase = 1;
        return false;
    }
    if (ai_can_act(ai_p) && ai->throw_in == 0) {
        ai->throw_in = 2;
        // After throwing, immediately switch to sword retrieval mindset
        ai->pattern_phase = 2;
        return false;
    }
    if (ai->pattern_phase == 2) {
        // Throw was queued — done, let unarmed logic take over
        return true;
    }
    return false;
}

static bool exec_bait_parry(AIBrain *ai, const Player *ai_p,
                             const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    float bait_dist = ai_attack_range() + 20.0f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    // Track human vertically too
    ai_follow_vertical(ai, ai_p, human_ty);

    if (perc->abs_dist > bait_dist + 15.0f)
        ai_move_toward(ai, ai_cx, human_cx, 10.0f);
    else if (perc->abs_dist < bait_dist - 15.0f)
        ai_move_away(ai, ai_cx, human_cx);

    if (perc->human_attacking && ai->parry_in == 0 && ai_can_act(ai_p)) {
        ai->parry_in = ai->reaction_delay;
        return false;
    }
    if (human->state == STATE_STUNNED && ai_can_act(ai_p) && ai->attack_in == 0) {
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        if (perc->abs_dist < ai_attack_range())
            ai->attack_in = 2;
        return true;
    }
    if (ai->pattern_timer <= 0) return true;
    return false;
}

static bool exec_spacing_dance(AIBrain *ai, const Player *ai_p,
                                const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        // Dance: alternate left/right every 20 frames
        int tick = (int)(ai->frame % 40);
        if (tick < 20) ai->hold_right = (human_cx > ai_cx);
        else           ai->hold_left  = (human_cx > ai_cx);
        // Also follow vertically if human is on a platform
        ai_follow_vertical(ai, ai_p, human_ty);
        if (ai->pattern_timer < 30) ai->pattern_phase = 1;
        return false;
    }
    ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
    if (perc->abs_dist < ai_attack_range() && ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

static bool exec_rush(AIBrain *ai, const Player *ai_p,
                      const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    float human_ty = human->body.pos.y;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    ai_chase(ai, ai_p, ai_cx, human_cx, human_ty, 5.0f);
    if (perc->abs_dist < ai_attack_range() && ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

static bool exec_throw_followup(AIBrain *ai, const Player *ai_p,
                                 const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        if (ai_can_act(ai_p) && ai->throw_in == 0) {
            ai->throw_in      = 2;
            ai->pattern_phase = 1;
            ai->pattern_timer = 90;
        }
        return false;
    }
    // Sword rebounding — position to catch
    if (perc->sword_rebounding && !ai_p->has_sword) {
        ai->trying_to_catch = true;
        float pred_x = perc->sword_x + perc->sword_vel_x * 8.0f * FIXED_DT;
        ai_move_toward(ai, ai_cx, pred_x, 20.0f);
        if (fabsf(perc->sword_x - ai_cx) < 80.0f)
            ai->hold_parry = true;
        return false;
    }
    if (ai_p->has_sword || ai->pattern_timer <= 0) return true;
    // No rebound — sword is flying or landed; go retrieve it
    if (!ai_p->has_sword) {
        (void)human_cx; // retrieval handled by PAT_SWORD_RETRIEVE
        return true;    // hand off to unarmed decision logic
    }
    return false;
}

// ---------------------------------------------------------------------------
// Unarmed patterns
// ---------------------------------------------------------------------------

// Run away, keep distance, jump if cornered
static bool exec_retreat_unarmed(AIBrain *ai, const Player *ai_p,
                                  const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (perc->abs_dist < 250.0f) {
        ai_move_away(ai, ai_cx, human_cx);
        if (perc->abs_dist < 120.0f && ai_p->body.on_ground && ai->jump_in == 0)
            ai->jump_in = 2;
    }
    return (ai_p->has_sword || ai->pattern_timer <= 0);
}

// ---------------------------------------------------------------------------
// Smart sword retrieval — the main unarmed pattern.
// Handles: grounded sword, airborne sword, human blocking path, and
// safely navigating platforms to reach the sword.
// ---------------------------------------------------------------------------
static bool exec_retrieve_sword(AIBrain *ai, const Player *ai_p,
                                 const Player *human, const AIPerception *perc) {
    if (ai_p->has_sword) return true;

    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    // --- Priority 1: catch if sword is rebounding toward us ---
    if (ai_should_catch(ai, ai_p, perc)) {
        float pred_x = perc->sword_x + perc->sword_vel_x * 10.0f * FIXED_DT;
        ai_move_toward(ai, ai_cx, pred_x, 15.0f);
        if (fabsf(perc->sword_x - ai_cx) < 100.0f)
            ai->hold_parry = true;
        return false;
    }

    // --- Priority 2: airborne sword that isn't a threat — move toward it ---
    if (perc->sword_in_air && !perc->sword_rebounding) {
        // Sword is flying away or neutral — predict landing and head there
        float pred_x = perc->sword_x + perc->sword_vel_x * 20.0f * FIXED_DT;
        float sword_ty = perc->sword_y;

        // Check if human is between us and sword
        bool human_between = (human_cx > fminf(ai_cx, pred_x)) &&
                             (human_cx < fmaxf(ai_cx, pred_x));

        if (human_between && human->has_sword && perc->abs_dist < 180.0f) {
            // Human is guarding the path — move away to create angle, then jump over
            ai_move_away(ai, ai_cx, human_cx);
            if (ai_p->body.on_ground && ai->jump_in == 0 && perc->abs_dist < 130.0f)
                ai->jump_in = 2;
        } else {
            ai_chase(ai, ai_p, ai_cx, pred_x, sword_ty, 10.0f);
        }
        return false;
    }

    // --- Priority 3: grounded sword ---
    if (!perc->ground_sword_exists) {
        // No sword visible at all — just stay safe
        if (perc->abs_dist < 200.0f) {
            ai_move_away(ai, ai_cx, human_cx);
            if (perc->abs_dist < 100.0f && ai_p->body.on_ground && ai->jump_in == 0)
                ai->jump_in = 2;
        }
        return false;
    }

    float sword_cx = perc->ground_sword_x;
    float sword_ty = perc->ground_sword_y;
    float to_sword = fabsf(sword_cx - ai_cx);

    // Human is between AI and sword AND is a threat (has sword, close)
    bool human_between = (human_cx > fminf(ai_cx, sword_cx)) &&
                         (human_cx < fmaxf(ai_cx, sword_cx));
    bool human_threatening = human->has_sword && perc->abs_dist < 200.0f;

    if (human_between && human_threatening) {
        // Strategy A: if sword is far enough, run the other way to lure human,
        // then double back — implemented by temporarily moving away
        if (to_sword > 300.0f) {
            // Lure: move away briefly, then switch back to retrieval
            ai_move_away(ai, ai_cx, human_cx);
            // Jump to a platform as a detour if we're on the ground
            if (ai_p->body.on_ground && ai->jump_in == 0 && ai->pattern_timer % 60 < 5)
                ai->jump_in = 2;
        } else {
            // Sword is close — human is guarding it. Jump over human.
            if (ai_p->body.on_ground && ai->jump_in == 0) {
                ai->jump_in = 2;
            }
            ai_move_toward(ai, ai_cx, sword_cx, 8.0f);
        }
        return false;
    }

    // Path is clear — move to sword, jumping over any vertical gap
    ai_chase(ai, ai_p, ai_cx, sword_cx, sword_ty, 8.0f);

    // If AI and sword are at similar Y (same ground level) just walk
    // If sword is on a platform above, ai_chase already handles the jump

    return false; // stays in this pattern until has_sword
}

// Position to catch rebounding sword
static bool exec_catch_wait(AIBrain *ai, const Player *ai_p,
                             const Player *human, const AIPerception *perc) {
    (void)human;
    float ai_cx = ai_p->body.pos.x + PLAYER_W*0.5f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (!perc->sword_in_air || !perc->sword_rebounding) return true;

    float pred_x = perc->sword_x + perc->sword_vel_x * 10.0f * FIXED_DT;
    ai_move_toward(ai, ai_cx, pred_x, 15.0f);

    if (fabsf(perc->sword_x - ai_cx) < 100.0f)
        ai->hold_parry = true;

    return (ai_p->has_sword);
}

// Dummy platform flee — just jumps and moves away (fallback)
static bool exec_platform_flee(AIBrain *ai, const Player *ai_p,
                                const Player *human, const AIPerception *perc) {
    (void)perc;
    float ai_cx    = ai_p->body.pos.x + PLAYER_W*0.5f;
    float human_cx = human->body.pos.x + PLAYER_W*0.5f;
    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    ai_move_away(ai, ai_cx, human_cx);
    if (ai_p->body.on_ground && ai->jump_in == 0 && ai->pattern_phase == 0) {
        ai->jump_in       = 2;
        ai->pattern_phase = 1;
    }
    return (ai_p->has_sword || ai->pattern_timer <= 0);
}

// ---------------------------------------------------------------------------
// Run a pattern, return done flag
// ---------------------------------------------------------------------------
static bool ai_run_pattern(AIBrain *ai, AIPattern pat,
                            const Player *ai_player, const Player *human,
                            const AIPerception *perc) {
    switch (pat) {
        case PAT_PRESSURE:       return exec_pressure(ai, ai_player, human, perc);
        case PAT_FEINT_ATTACK:   return exec_feint(ai, ai_player, human, perc);
        case PAT_JUMP_ATTACK:    return exec_jump_attack(ai, ai_player, human, perc);
        case PAT_CROUCH_POKE:    return exec_crouch_poke(ai, ai_player, human, perc);
        case PAT_THROW_FOLLOWUP: return exec_throw_followup(ai, ai_player, human, perc);
        case PAT_BAIT_PARRY:     return exec_bait_parry(ai, ai_player, human, perc);
        case PAT_SPACING_DANCE:  return exec_spacing_dance(ai, ai_player, human, perc);
        case PAT_RUSH_ATTACK:    return exec_rush(ai, ai_player, human, perc);
        case PAT_RETREAT_THROW:  return exec_retreat_throw(ai, ai_player, human, perc);
        case PAT_RETREAT_UNARMED:return exec_retreat_unarmed(ai, ai_player, human, perc);
        case PAT_PLATFORM_FLEE:  return exec_platform_flee(ai, ai_player, human, perc);
        case PAT_SWORD_RETRIEVE: return exec_retrieve_sword(ai, ai_player, human, perc);
        case PAT_CATCH_WAIT:     return exec_catch_wait(ai, ai_player, human, perc);
        default:                 return true;
    }
}

// ---------------------------------------------------------------------------
// Top-level update
// ---------------------------------------------------------------------------
void ai_update(AIBrain *ai,
               const Player *ai_player,
               const Player *human,
               const ThrowingSword *swords, int num_swords,
               Input *out) {

    ai->frame++;
    ai_tick_timers(ai);

    ai->hold_left   = false;
    ai->hold_right  = false;
    ai->hold_crouch = false;
    ai->hold_parry  = false;

    if (ai_player->state == STATE_DEAD) {
        ai_build_input(ai, out, ai->frame);
        return;
    }

    AIPerception perc = ai_perceive(ai_player, human, swords, num_swords);

    // Aggression drift
    if (ai->aggression_timer <= 0) {
        float target_agg = 40.0f + rng_f(&ai->rng) * 60.0f;
        ai->aggression   = ai->aggression * 0.7f + target_agg * 0.3f;
        ai->aggression_timer = 120 + rng_i(&ai->rng, 120);
    }

    // -----------------------------------------------------------------------
    // PRIORITY OVERRIDES
    // -----------------------------------------------------------------------

    // 1. Incoming sword — parry
    if (ai_sword_incoming(ai, ai_player, &perc) &&
        ai_player->has_sword && ai_can_act(ai_player) && ai->parry_in == 0) {
        ai->parry_in = ai->reaction_delay;
    }

    // 2. Human swinging close — parry
    if (perc.human_attacking && perc.abs_dist < ai_attack_range() + 20.0f &&
        ai_can_act(ai_player) && ai->parry_in == 0 && ai_player->has_sword) {
        ai->parry_in = ai->reaction_delay;
    }

    // 3. Rebounding sword — catch
    if (ai_should_catch(ai, ai_player, &perc) &&
        ai->current_pattern != PAT_CATCH_WAIT &&
        ai->current_pattern != PAT_THROW_FOLLOWUP) {
        ai->current_pattern  = PAT_CATCH_WAIT;
        ai->pattern_timer    = 90;
        ai->pattern_phase    = 0;
        ai->queue_len        = 0;
    }

    // 4. Unarmed and sword exists — always switch to retrieval unless catching
    if (!ai_player->has_sword &&
        ai->current_pattern != PAT_CATCH_WAIT &&
        ai->current_pattern != PAT_SWORD_RETRIEVE &&
        (perc.ground_sword_exists || perc.sword_in_air)) {
        ai->current_pattern  = PAT_SWORD_RETRIEVE;
        ai->pattern_timer    = 300;
        ai->pattern_phase    = 0;
        ai->queue_len        = 0;
    }

    // 5. Just regained sword mid-pattern — return to armed logic
    if (ai_player->has_sword &&
        (ai->current_pattern == PAT_SWORD_RETRIEVE ||
         ai->current_pattern == PAT_RETREAT_UNARMED ||
         ai->current_pattern == PAT_PLATFORM_FLEE ||
         ai->current_pattern == PAT_CATCH_WAIT)) {
        ai->current_pattern  = PAT_PRESSURE;
        ai->pattern_timer    = 60;
        ai->pattern_phase    = 0;
        ai->queue_len        = 0;
        ai->decision_cooldown = 10;
    }

    // 6. Punish stunned human
    if (human->state == STATE_STUNNED && ai_player->has_sword &&
        ai_can_act(ai_player) && perc.abs_dist < ai_attack_range() &&
        ai->attack_in == 0) {
        ai->attack_in = ai->reaction_delay;
    }

    // -----------------------------------------------------------------------
    // Pattern management
    // -----------------------------------------------------------------------
    if (ai->decision_cooldown <= 0) {
        bool done = ai_run_pattern(ai, ai->current_pattern, ai_player, human, &perc);

        if (done || ai->pattern_timer <= 0) {
            ai_remember_pattern(ai, ai->current_pattern);

            if (ai->queue_head < ai->queue_len) {
                ai->current_pattern = ai->pattern_queue[ai->queue_head++];
            } else {
                int combo_len = 2 + rng_i(&ai->rng, 3);
                AIPattern combo[4];
                for (int c = 0; c < combo_len; c++) {
                    AIPattern p;
                    int tries = 0;
                    do {
                        p = ai_choose_pattern(ai, ai_player, human, &perc);
                        tries++;
                    } while (tries < 5 && c > 0 && combo[c-1] == p);
                    combo[c] = p;
                }
                ai_queue(ai, combo, combo_len);
                ai->current_pattern = ai->pattern_queue[ai->queue_head++];
            }

            ai->pattern_phase    = 0;
            ai->pattern_timer    = 90 + rng_i(&ai->rng, 60);
            ai->decision_cooldown = 8 + rng_i(&ai->rng, 12);
        }
    } else {
        // On cooldown — keep executing movement from current pattern
        ai_run_pattern(ai, ai->current_pattern, ai_player, human, &perc);
    }

    ai_build_input(ai, out, ai->frame);
}
