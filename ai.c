#include "ai.h"
#include "combat.h"
#include "physics.h"
#include <math.h>
#include <string.h>
#include <float.h>

// ---------------------------------------------------------------------------
// RNG — simple xorshift32, seeded in ai_init
// ---------------------------------------------------------------------------
static uint32_t rng_next(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}
// Returns float in [0,1)
static float rng_f(uint32_t *s) {
    return (float)(rng_next(s) & 0xFFFFFF) / (float)0x1000000;
}
// Returns int in [0, n)
static int rng_i(uint32_t *s, int n) {
    return (int)(rng_f(s) * (float)n);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void ai_init(AIBrain *ai) {
    memset(ai, 0, sizeof(*ai));
    ai->rng              = 0xDEADBEEF;
    ai->aggression       = 60.0f;
    ai->aggression_timer = 180;
    ai->reaction_delay   = 6;   // ~100ms reaction window
    ai->preferred_dist   = 120.0f;
    ai->decision_cooldown = 20;
    ai->avg_attack_interval = 60;
    ai->current_pattern  = PAT_PRESSURE;
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
    // Sword tip reach: SWORD_LENGTH plus a bit of body width
    return SWORD_LENGTH + PLAYER_W * 0.5f + 10.0f;
}

// Perceive the world from AI's perspective
static AIPerception ai_perceive(const Player *ai_p,
                                const Player *human,
                                const ThrowingSword *swords, int num_swords) {
    AIPerception p;
    memset(&p, 0, sizeof(p));

    p.dist  = (human->body.pos.x + PLAYER_W * 0.5f) -
              (ai_p->body.pos.x  + PLAYER_W * 0.5f);
    p.abs_dist       = fabsf(p.dist);
    p.human_vel_x    = human->body.vel.x;
    p.human_attacking= (human->state == STATE_ATTACK);
    p.human_parrying = (human->state == STATE_PARRY);
    p.human_has_sword= human->has_sword;

    // Find nearest active thrown sword and nearest grounded sword
    p.ground_sword_x = FLT_MAX;
    p.sword_x        = FLT_MAX;
    float best_air_dist   = FLT_MAX;
    float best_gnd_dist   = FLT_MAX;

    for (int i = 0; i < num_swords; i++) {
        const ThrowingSword *s = &swords[i];
        if (!s->active) continue;

        float sd = fabsf(s->pos.x - (ai_p->body.pos.x + PLAYER_W * 0.5f));

        // Is it effectively grounded and still?
        bool grounded = (s->pos.y >= g_ground_y() - 2.0f) &&
                        fabsf(s->vel.x) < 5.0f && fabsf(s->vel.y) < 5.0f;

        if (grounded) {
            if (sd < best_gnd_dist) {
                best_gnd_dist          = sd;
                p.ground_sword_x       = s->pos.x;
                p.ground_sword_y       = s->pos.y;
                p.ground_sword_exists  = true;
            }
        } else {
            if (sd < best_air_dist) {
                best_air_dist    = sd;
                p.sword_x        = s->pos.x;
                p.sword_y        = s->pos.y;
                p.sword_vel_x    = s->vel.x;
                p.sword_owner    = s->owner;
                p.sword_in_air   = true;
                p.sword_rebounding = s->rebounding;
            }
        }
    }
    return p;
}

// Queue a pattern sequence; overwrites existing queue
static void ai_queue(AIBrain *ai, const AIPattern *pats, int n) {
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) ai->pattern_queue[i] = pats[i];
    ai->queue_len  = n;
    ai->queue_head = 0;
}

// Remember this pattern for anti-repetition
static void ai_remember_pattern(AIBrain *ai, AIPattern pat) {
    ai->last_patterns[ai->last_pattern_idx % 3] = pat;
    ai->last_pattern_idx++;
}

// Check if pattern was used recently
static bool ai_pattern_recent(const AIBrain *ai, AIPattern pat) {
    for (int i = 0; i < 3; i++)
        if (ai->last_patterns[i] == pat) return true;
    return false;
}

// Tick down all pending input timers
static void ai_tick_timers(AIBrain *ai) {
    if (ai->jump_in   > 0) ai->jump_in--;
    if (ai->attack_in > 0) ai->attack_in--;
    if (ai->parry_in  > 0) ai->parry_in--;
    if (ai->throw_in  > 0) ai->throw_in--;
    if (ai->feint_timer  > 0) ai->feint_timer--;
    if (ai->catch_timer  > 0) ai->catch_timer--;
    if (ai->pattern_timer > 0) ai->pattern_timer--;
    if (ai->decision_cooldown > 0) ai->decision_cooldown--;
    if (ai->aggression_timer  > 0) ai->aggression_timer--;
    if (ai->react_timer       > 0) ai->react_timer--;
}

// Assemble booleans into Input struct
static void ai_build_input(AIBrain *ai, Input *out, uint32_t frame) {
    memset(out, 0, sizeof(*out));
    out->frame       = frame;
    out->left        = ai->hold_left;
    out->right       = ai->hold_right;
    out->crouch      = ai->hold_crouch;
    out->parry       = ai->hold_parry;
    out->jump        = (ai->jump_in   == 1);
    out->attack      = (ai->attack_in == 1);
    out->throw_weapon= (ai->throw_in  == 1);
    // parry is also triggered as edge if parry_in fires
    if (ai->parry_in == 1) out->parry = true;
}

// ---------------------------------------------------------------------------
// Movement helpers — set hold_left/right to move toward/away from target x
// ---------------------------------------------------------------------------
static void ai_move_toward(AIBrain *ai, float ai_cx, float target_x, float tolerance) {
    float d = target_x - ai_cx;
    ai->hold_left  = false;
    ai->hold_right = false;
    if (d > tolerance)  ai->hold_right = true;
    else if (d < -tolerance) ai->hold_left = true;
}

static void ai_move_away(AIBrain *ai, float ai_cx, float human_cx) {
    ai->hold_left  = (human_cx > ai_cx);
    ai->hold_right = (human_cx < ai_cx);
}

// ---------------------------------------------------------------------------
// Threat detection — is there a sword coming toward us this frame?
// ---------------------------------------------------------------------------
static bool ai_sword_incoming(const AIBrain *ai, const Player *ai_p,
                               const AIPerception *perc) {
    (void)ai;
    if (!perc->sword_in_air) return false;
    // Sword moving toward AI's x position
    float ai_cx = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float to_ai = ai_cx - perc->sword_x;
    // Sword vel_x sign matches direction to AI
    return (to_ai * perc->sword_vel_x > 0.0f) &&
           (fabsf(perc->sword_x - ai_cx) < 400.0f);
}

// Is the sword approaching fast and close enough to warrant a catch attempt?
static bool ai_should_catch(const AIBrain *ai, const Player *ai_p,
                             const AIPerception *perc) {
    (void)ai;
    if (!ai_p->has_sword && perc->sword_in_air && perc->sword_rebounding) {
        float ai_cx = ai_p->body.pos.x + PLAYER_W * 0.5f;
        float dist  = fabsf(perc->sword_x - ai_cx);
        float to_ai = ai_cx - perc->sword_x;
        bool heading_toward = (to_ai * perc->sword_vel_x > 0.0f);
        return heading_toward && dist < 350.0f;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Decision maker — choose next pattern based on situation
// ---------------------------------------------------------------------------
static AIPattern ai_choose_pattern(AIBrain *ai, const Player *ai_p,
                                   const Player *human,
                                   const AIPerception *perc) {
    float agg = ai->aggression;

    // --- Unarmed logic ---
    if (!ai_p->has_sword) {
        // Rebounding sword flying back? Try to catch it
        if (ai_should_catch(ai, ai_p, perc))
            return PAT_CATCH_WAIT;
        // Sword on the ground nearby?
        if (perc->ground_sword_exists) {
            float ai_cx = ai_p->body.pos.x + PLAYER_W * 0.5f;
            float sword_dist = fabsf(perc->ground_sword_x - ai_cx);
            // Human is between AI and sword — flee to platform first
            float human_cx = human->body.pos.x + PLAYER_W * 0.5f;
            float human_to_sword = fabsf(human_cx - perc->ground_sword_x);
            if (human_to_sword < sword_dist && human->has_sword)
                return PAT_PLATFORM_FLEE;
            return PAT_SWORD_RETRIEVE;
        }
        // No sword anywhere — just survive
        return perc->abs_dist < 200.0f ? PAT_PLATFORM_FLEE : PAT_RETREAT_UNARMED;
    }

    // --- Armed logic ---

    // Human is attacking us — parry
    if (perc->human_attacking && perc->abs_dist < ai_attack_range() + 30.0f)
        return PAT_BAIT_PARRY;

    // Sword incoming — dodge / parry
    if (ai_sword_incoming(ai, ai_p, perc))
        return PAT_BAIT_PARRY;

    // Choose based on distance and aggression
    float roll = rng_f(&ai->rng) * 100.0f;

    if (perc->abs_dist > 350.0f) {
        // Far away — close the gap or throw
        if (roll < agg * 0.4f) return PAT_RUSH_ATTACK;
        if (roll < agg * 0.6f && !ai_pattern_recent(ai, PAT_RETREAT_THROW))
            return PAT_RETREAT_THROW;
        return PAT_PRESSURE;
    }

    if (perc->abs_dist > 180.0f) {
        // Mid range — varied approaches
        if (roll < 20.0f && !ai_pattern_recent(ai, PAT_JUMP_ATTACK))
            return PAT_JUMP_ATTACK;
        if (roll < 40.0f && !ai_pattern_recent(ai, PAT_FEINT_ATTACK))
            return PAT_FEINT_ATTACK;
        if (roll < 55.0f) return PAT_PRESSURE;
        if (roll < 70.0f && !ai_pattern_recent(ai, PAT_SPACING_DANCE))
            return PAT_SPACING_DANCE;
        if (roll < 80.0f && !ai_pattern_recent(ai, PAT_RETREAT_THROW))
            return PAT_RETREAT_THROW;
        return PAT_BAIT_PARRY;
    }

    // Close range — high-pressure mix
    if (roll < 25.0f && !ai_pattern_recent(ai, PAT_CROUCH_POKE))
        return PAT_CROUCH_POKE;
    if (roll < 45.0f) return PAT_PRESSURE;
    if (roll < 60.0f) return PAT_BAIT_PARRY;
    if (roll < 75.0f && !ai_pattern_recent(ai, PAT_JUMP_ATTACK))
        return PAT_JUMP_ATTACK;
    if (roll < 85.0f && !ai_pattern_recent(ai, PAT_FEINT_ATTACK))
        return PAT_FEINT_ATTACK;
    return PAT_THROW_FOLLOWUP;
}

// ---------------------------------------------------------------------------
// Pattern executors — each returns true when the pattern is complete
// ---------------------------------------------------------------------------

// Move toward human until in attack range, then attack
static bool exec_pressure(AIBrain *ai, const Player *ai_p,
                          const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;
    float range    = ai_attack_range();

    ai->hold_left = ai->hold_right = ai->hold_crouch = false;
    ai->hold_parry = false;

    if (perc->abs_dist > range - 10.0f) {
        // Close in
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        ai->pattern_timer = 60;
        return false;
    }
    // In range — attack
    if (ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 2 + rng_i(&ai->rng, 3); // slight delay for realism
        return true;
    }
    return false;
}

// Approach, stop just outside range, wait 1-2 frames (feint), then attack
static bool exec_feint(AIBrain *ai, const Player *ai_p,
                       const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;
    float feint_range = ai_attack_range() + 30.0f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = false;
    ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        // Phase 0: approach to feint range
        if (perc->abs_dist > feint_range) {
            ai_move_toward(ai, ai_cx, human_cx, 5.0f);
            return false;
        }
        // Arrived — stop and start feint timer
        ai->feinting    = true;
        ai->feint_timer = 8 + rng_i(&ai->rng, 8);
        ai->pattern_phase = 1;
        return false;
    }
    if (ai->pattern_phase == 1) {
        // Phase 1: stand still (feint)
        if (ai->feint_timer > 0) return false;
        // Feint done — now actually move in and attack
        ai->pattern_phase = 2;
        return false;
    }
    // Phase 2: close and attack
    if (perc->abs_dist > ai_attack_range() - 5.0f) {
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        return false;
    }
    if (ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

// Jump toward human and attack in the air or on landing
static bool exec_jump_attack(AIBrain *ai, const Player *ai_p,
                              const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = false;
    ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        // Jump
        if (ai_p->body.on_ground && ai->jump_in == 0) {
            ai->jump_in       = 2;
            ai->pattern_phase = 1;
        }
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        return false;
    }
    // In air — move toward human, attack when close enough
    ai_move_toward(ai, ai_cx, human_cx, 5.0f);
    if (perc->abs_dist < ai_attack_range() + 20.0f && ai->attack_in == 0) {
        ai->attack_in = 2;
        return true;
    }
    // Timed out
    if (ai_p->body.on_ground && ai->pattern_phase == 1) return true;
    return false;
}

// Slow deliberate walk-in then attack — replaces crouch poke since crouch is drop-only
static bool exec_crouch_poke(AIBrain *ai, const Player *ai_p,
                              const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    // Walk in slowly (no sprint) then attack at tight range
    float tight_range = ai_attack_range() - 15.0f;
    if (perc->abs_dist > tight_range) {
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        return false;
    }
    if (ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

// Back up to mid range, then throw
static bool exec_retreat_throw(AIBrain *ai, const Player *ai_p,
                                const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;
    float retreat_dist = 250.0f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = false;
    ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        // Back away to retreat_dist
        if (perc->abs_dist < retreat_dist) {
            ai_move_away(ai, ai_cx, human_cx);
            return false;
        }
        ai->pattern_phase = 1;
        return false;
    }
    // Throw
    if (ai_can_act(ai_p) && ai->throw_in == 0) {
        ai->throw_in = 2;
        return true;
    }
    return false;
}

// Stand at edge of attack range; if human swings, parry-counter
static bool exec_bait_parry(AIBrain *ai, const Player *ai_p,
                             const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;
    float bait_dist = ai_attack_range() + 20.0f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = false;
    ai->hold_parry = false;

    // Stay at bait_dist
    if (perc->abs_dist > bait_dist + 15.0f)
        ai_move_toward(ai, ai_cx, human_cx, 10.0f);
    else if (perc->abs_dist < bait_dist - 15.0f)
        ai_move_away(ai, ai_cx, human_cx);

    // React to human attacking
    if (perc->human_attacking && ai->parry_in == 0 && ai_can_act(ai_p)) {
        ai->parry_in = ai->reaction_delay;
        return false;
    }
    // After parry lands (stunned human), follow with an attack
    if (human->state == STATE_STUNNED && ai_can_act(ai_p) && ai->attack_in == 0) {
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
        if (perc->abs_dist < ai_attack_range())
            ai->attack_in = 2;
        return true;
    }
    // Time limit on this pattern
    if (ai->pattern_timer <= 0) return true;
    return false;
}

// Walk back and forth at medium distance, then burst in to attack
static bool exec_spacing_dance(AIBrain *ai, const Player *ai_p,
                                const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        // Dance: alternate move left/right every 20 frames
        int tick = (int)(ai->frame % 40);
        if (tick < 20) ai->hold_right = (human_cx > ai_cx);
        else           ai->hold_left  = (human_cx > ai_cx);

        if (ai->pattern_timer < 30) ai->pattern_phase = 1;
        return false;
    }
    // Burst in and attack
    ai_move_toward(ai, ai_cx, human_cx, 5.0f);
    if (perc->abs_dist < ai_attack_range() && ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

// Sprint hard and attack quickly
static bool exec_rush(AIBrain *ai, const Player *ai_p,
                      const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;
    ai_move_toward(ai, ai_cx, human_cx, 5.0f);

    if (perc->abs_dist < ai_attack_range() && ai_can_act(ai_p) && ai->attack_in == 0) {
        ai->attack_in = 1;
        return true;
    }
    return false;
}

// Throw, then follow up: if throw is parried and sword rebounds, try to catch
static bool exec_throw_followup(AIBrain *ai, const Player *ai_p,
                                 const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (ai->pattern_phase == 0) {
        // Throw
        if (ai_can_act(ai_p) && ai->throw_in == 0) {
            ai->throw_in      = 2;
            ai->pattern_phase = 1;
            ai->pattern_timer = 90;
        }
        return false;
    }
    // Phase 1: after throw, watch for rebound and position to catch
    if (perc->sword_rebounding && !ai_p->has_sword) {
        ai->trying_to_catch = true;
        // Move under the rebounding sword's projected landing
        float pred_x = perc->sword_x + perc->sword_vel_x * 8.0f * FIXED_DT;
        ai_move_toward(ai, ai_cx, pred_x, 20.0f);
        // Raise parry when sword is close
        if (fabsf(perc->sword_x - ai_cx) < 80.0f)
            ai->hold_parry = true;
        return false;
    }
    // Got sword back or timeout
    if (ai_p->has_sword || ai->pattern_timer <= 0) return true;
    // Otherwise close in on human
    if (!ai_p->has_sword) {
        ai_move_toward(ai, ai_cx, human_cx, 5.0f);
    }
    return false;
}

// --- Unarmed patterns ---

// Run away from human, maintain safe distance
static bool exec_retreat_unarmed(AIBrain *ai, const Player *ai_p,
                                  const Player *human, const AIPerception *perc) {
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (perc->abs_dist < 250.0f) {
        ai_move_away(ai, ai_cx, human_cx);
        // Jump if human is very close
        if (perc->abs_dist < 120.0f && ai_p->body.on_ground && ai->jump_in == 0)
            ai->jump_in = 2;
    }
    // Done if we got a sword somehow or timer expired
    return (ai_p->has_sword || ai->pattern_timer <= 0);
}

// Jump to a platform to escape the human
static bool exec_platform_flee(AIBrain *ai, const Player *ai_p,
                                const Player *human, const AIPerception *perc) {
    (void)perc;
    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    // Move away from human and jump
    ai_move_away(ai, ai_cx, human_cx);
    if (ai_p->body.on_ground && ai->jump_in == 0 && ai->pattern_phase == 0) {
        ai->jump_in       = 2;
        ai->pattern_phase = 1;
    }
    return (ai_p->has_sword || ai->pattern_timer <= 0);
}

// Navigate toward the nearest grounded sword and pick it up
static bool exec_retrieve_sword(AIBrain *ai, const Player *ai_p,
                                 const Player *human, const AIPerception *perc) {
    if (!perc->ground_sword_exists) return true;  // nothing to retrieve

    float ai_cx    = ai_p->body.pos.x + PLAYER_W * 0.5f;
    float human_cx = human->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    float sword_cx = perc->ground_sword_x;
    float to_sword = fabsf(sword_cx - ai_cx);

    // If human is blocking the path, jump over
    bool human_between = (human_cx > fminf(ai_cx, sword_cx)) &&
                         (human_cx < fmaxf(ai_cx, sword_cx));
    if (human_between && human->has_sword && to_sword > 60.0f) {
        if (ai_p->body.on_ground && ai->jump_in == 0)
            ai->jump_in = 2;
    }

    ai_move_toward(ai, ai_cx, sword_cx, 8.0f);

    return (ai_p->has_sword);
}

// Position to catch the rebounding sword with parry input
static bool exec_catch_wait(AIBrain *ai, const Player *ai_p,
                             const Player *human, const AIPerception *perc) {
    (void)human;
    float ai_cx = ai_p->body.pos.x + PLAYER_W * 0.5f;

    ai->hold_left = ai->hold_right = ai->hold_crouch = ai->hold_parry = false;

    if (!perc->sword_in_air || !perc->sword_rebounding) {
        // Sword landed or disappeared
        return true;
    }

    // Predict where sword will be in a few frames and position under it
    float pred_x = perc->sword_x + perc->sword_vel_x * 10.0f * FIXED_DT;
    ai_move_toward(ai, ai_cx, pred_x, 15.0f);

    // When sword is close enough, raise parry
    float dist_to_sword = fabsf(perc->sword_x - ai_cx);
    if (dist_to_sword < 100.0f) {
        ai->hold_parry = true;
    }

    return (ai_p->has_sword);
}

// ---------------------------------------------------------------------------
// Top-level AI update
// ---------------------------------------------------------------------------
void ai_update(AIBrain *ai,
               const Player *ai_player,
               const Player *human,
               const ThrowingSword *swords, int num_swords,
               Input *out) {

    ai->frame++;
    ai_tick_timers(ai);

    // Reset movement holds each frame (patterns re-assert them)
    ai->hold_left   = false;
    ai->hold_right  = false;
    ai->hold_crouch = false;
    ai->hold_parry  = false;

    // Dead — do nothing
    if (ai_player->state == STATE_DEAD) {
        ai_build_input(ai, out, ai->frame);
        return;
    }

    // Perceive world
    AIPerception perc = ai_perceive(ai_player, human, swords, num_swords);

    // -----------------------------------------------------------------------
    // Aggression drift: slowly oscillate to keep the AI varied
    // -----------------------------------------------------------------------
    if (ai->aggression_timer <= 0) {
        float target_agg = 40.0f + rng_f(&ai->rng) * 60.0f;
        ai->aggression   = ai->aggression * 0.7f + target_agg * 0.3f;
        ai->aggression_timer = 120 + rng_i(&ai->rng, 120);
    }

    // -----------------------------------------------------------------------
    // PRIORITY OVERRIDES — immediate reactions regardless of current pattern
    // -----------------------------------------------------------------------

    // 1. Sword incoming and we have a sword — raise parry NOW
    if (ai_sword_incoming(ai, ai_player, &perc) &&
        ai_player->has_sword && ai_can_act(ai_player) &&
        ai->parry_in == 0) {
        ai->parry_in = ai->reaction_delay;
    }

    // 2. Human swinging at close range — parry
    if (perc.human_attacking && perc.abs_dist < ai_attack_range() + 20.0f &&
        ai_can_act(ai_player) && ai->parry_in == 0 && ai_player->has_sword) {
        ai->parry_in = ai->reaction_delay;
    }

    // 3. Rebounding sword coming back while unarmed — switch to catch pattern
    if (ai_should_catch(ai, ai_player, &perc) &&
        ai->current_pattern != PAT_CATCH_WAIT &&
        ai->current_pattern != PAT_THROW_FOLLOWUP) {
        ai->current_pattern  = PAT_CATCH_WAIT;
        ai->pattern_timer    = 90;
        ai->pattern_phase    = 0;
        ai->queue_len        = 0;
    }

    // 4. After a successful parry stun, punish with attack
    if (human->state == STATE_STUNNED && ai_player->has_sword &&
        ai_can_act(ai_player) && perc.abs_dist < ai_attack_range() &&
        ai->attack_in == 0) {
        ai->attack_in = ai->reaction_delay;
    }

    // -----------------------------------------------------------------------
    // Pattern management
    // -----------------------------------------------------------------------
    if (ai->decision_cooldown <= 0) {
        bool pattern_done = false;

        // Execute current pattern
        switch (ai->current_pattern) {
            case PAT_PRESSURE:
                pattern_done = exec_pressure(ai, ai_player, human, &perc); break;
            case PAT_FEINT_ATTACK:
                pattern_done = exec_feint(ai, ai_player, human, &perc); break;
            case PAT_JUMP_ATTACK:
                pattern_done = exec_jump_attack(ai, ai_player, human, &perc); break;
            case PAT_CROUCH_POKE:
                pattern_done = exec_crouch_poke(ai, ai_player, human, &perc); break;
            case PAT_THROW_FOLLOWUP:
                pattern_done = exec_throw_followup(ai, ai_player, human, &perc); break;
            case PAT_BAIT_PARRY:
                pattern_done = exec_bait_parry(ai, ai_player, human, &perc); break;
            case PAT_SPACING_DANCE:
                pattern_done = exec_spacing_dance(ai, ai_player, human, &perc); break;
            case PAT_RUSH_ATTACK:
                pattern_done = exec_rush(ai, ai_player, human, &perc); break;
            case PAT_RETREAT_THROW:
                pattern_done = exec_retreat_throw(ai, ai_player, human, &perc); break;
            case PAT_RETREAT_UNARMED:
                pattern_done = exec_retreat_unarmed(ai, ai_player, human, &perc); break;
            case PAT_PLATFORM_FLEE:
                pattern_done = exec_platform_flee(ai, ai_player, human, &perc); break;
            case PAT_SWORD_RETRIEVE:
                pattern_done = exec_retrieve_sword(ai, ai_player, human, &perc); break;
            case PAT_CATCH_WAIT:
                pattern_done = exec_catch_wait(ai, ai_player, human, &perc); break;
            default:
                pattern_done = true; break;
        }

        if (pattern_done || ai->pattern_timer <= 0) {
            ai_remember_pattern(ai, ai->current_pattern);

            // Advance queue or pick new pattern
            if (ai->queue_head < ai->queue_len) {
                ai->current_pattern = ai->pattern_queue[ai->queue_head++];
            } else {
                // Build a combo sequence: 2-4 patterns chained together
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

            // Reset pattern state
            ai->pattern_phase = 0;
            ai->pattern_timer = 90 + rng_i(&ai->rng, 60);
            ai->decision_cooldown = 8 + rng_i(&ai->rng, 12);
        }
    } else {
        // Still on cooldown — keep executing current pattern's movement
        switch (ai->current_pattern) {
            case PAT_PRESSURE:
                exec_pressure(ai, ai_player, human, &perc); break;
            case PAT_FEINT_ATTACK:
                exec_feint(ai, ai_player, human, &perc); break;
            case PAT_JUMP_ATTACK:
                exec_jump_attack(ai, ai_player, human, &perc); break;
            case PAT_CROUCH_POKE:
                exec_crouch_poke(ai, ai_player, human, &perc); break;
            case PAT_THROW_FOLLOWUP:
                exec_throw_followup(ai, ai_player, human, &perc); break;
            case PAT_BAIT_PARRY:
                exec_bait_parry(ai, ai_player, human, &perc); break;
            case PAT_SPACING_DANCE:
                exec_spacing_dance(ai, ai_player, human, &perc); break;
            case PAT_RUSH_ATTACK:
                exec_rush(ai, ai_player, human, &perc); break;
            case PAT_RETREAT_THROW:
                exec_retreat_throw(ai, ai_player, human, &perc); break;
            case PAT_RETREAT_UNARMED:
                exec_retreat_unarmed(ai, ai_player, human, &perc); break;
            case PAT_PLATFORM_FLEE:
                exec_platform_flee(ai, ai_player, human, &perc); break;
            case PAT_SWORD_RETRIEVE:
                exec_retrieve_sword(ai, ai_player, human, &perc); break;
            case PAT_CATCH_WAIT:
                exec_catch_wait(ai, ai_player, human, &perc); break;
            default: break;
        }
    }

    ai_build_input(ai, out, ai->frame);
}
