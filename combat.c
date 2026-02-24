#include "combat.h"
#include "physics.h"
#include <math.h>

// Check if attacker's weapon hitbox hits defender's hurtbox
// Returns true if kill, and also handles parry
static HitResult check_hit(Player *attacker, Player *defender) {
    Rect weapon = player_sword_rect(attacker);
    if (weapon.w == 0.0f) return HIT_NONE;  // no active hitbox

    Rect parry = player_parry_rect(defender);
    Rect body  = defender->hurtbox;

    // Check parry first (parry takes priority)
    if (parry.w > 0.0f && rect_overlap(weapon, parry)) {
        return HIT_PARRY;
    }

    // Check body hit
    if (rect_overlap(weapon, body)) {
        return HIT_BODY;
    }

    return HIT_NONE;
}

// Check if two swords clash (weapon hitboxes overlap)
static bool check_clash(Player *p0, Player *p1) {
    Rect w0 = player_sword_rect(p0);
    Rect w1 = player_sword_rect(p1);
    if (w0.w == 0.0f || w1.w == 0.0f) return false;
    return rect_overlap(w0, w1);
}

CombatResult combat_resolve(Player *p0, Player *p1, ThrowingSword *swords, int num_swords) {
    CombatResult result;
    result.p0_hit = HIT_NONE;
    result.p1_hit = HIT_NONE;
    result.sword_clash = false;

    if (p0->state == STATE_DEAD || p1->state == STATE_DEAD) return result;

    // Check sword clash first
    if (check_clash(p0, p1)) {
        result.sword_clash = true;
        // Cancel both attacks with brief stun
        if (p0->state == STATE_ATTACK) {
            p0->state = STATE_STUNNED;
            p0->stun_timer = 8;
            p0->state_timer = 0;
        }
        if (p1->state == STATE_ATTACK) {
            p1->state = STATE_STUNNED;
            p1->stun_timer = 8;
            p1->state_timer = 0;
        }
        return result;  // clash cancels further hits
    }

    // Check p0 hitting p1
    HitResult h01 = check_hit(p0, p1);
    // Check p1 hitting p0
    HitResult h10 = check_hit(p1, p0);

    // Resolve p0 -> p1
    if (h01 == HIT_PARRY) {
        result.p1_hit = HIT_PARRY;
        // p0 is stunned (attacker got parried)
        p0->state = STATE_STUNNED;
        p0->stun_timer = STUN_FRAMES;
        p0->state_timer = 0;
        // p1 gets recovery advantage (small knockback away)
        p1->body.vel.x += (float)p1->facing * 80.0f;
    } else if (h01 == HIT_BODY) {
        result.p1_hit = HIT_BODY;
        player_kill(p1);
    }

    // Resolve p1 -> p0 (only if p0 wasn't already killed above, and only if it actually attacked)
    if (h10 == HIT_PARRY && p1->state != STATE_DEAD) {
        result.p0_hit = HIT_PARRY;
        p1->state = STATE_STUNNED;
        p1->stun_timer = STUN_FRAMES;
        p1->state_timer = 0;
        p0->body.vel.x += (float)p0->facing * 80.0f;
    } else if (h10 == HIT_BODY && p0->state != STATE_DEAD) {
        result.p0_hit = HIT_BODY;
        player_kill(p0);
    }

    // Check thrown swords
    for (int i = 0; i < num_swords; i++) {
        ThrowingSword *s = &swords[i];
        if (!s->active) continue;
        if (s->hit_cooldown > 0) { s->hit_cooldown--; continue; }

        // Hitbox: 16x16 centered on sword tip position (matches melee weapon hitbox)
        Rect sword_rect;
        sword_rect.x = s->pos.x - 8;
        sword_rect.y = s->pos.y - 8;
        sword_rect.w = 16;
        sword_rect.h = 16;

        Player *targets[2] = {p0, p1};
        for (int t = 0; t < 2; t++) {
            Player *target = targets[t];
            if (target->id == s->owner) continue;  // can't hit current owner
            if (target->state == STATE_DEAD) continue;

            Rect parry = player_parry_rect(target);

            // --- CATCH: rebounding sword + disarmed original thrower presses parry ---
            // If the sword is flying back and the disarmed thrower has their parry box
            // active and the sword overlaps it, they catch it cleanly instead of dying.
            if (s->rebounding && !target->has_sword && parry.w > 0.0f && rect_overlap(sword_rect, parry)) {
                target->has_sword   = true;
                s->active           = false;
                // Small momentum boost — satisfying "caught it!" feel
                target->body.vel.x += (float)target->facing * 60.0f;
                break;
            }

            if (parry.w > 0.0f && rect_overlap(sword_rect, parry)) {
                // --- PARRY REBOUND ---
                // Reverse horizontal velocity and boost it slightly for satisfying feel
                s->vel.x = -s->vel.x * 1.1f;
                // Give a slight upward kick so it doesn't immediately hit the ground
                if (s->vel.y > 0.0f) s->vel.y = -s->vel.y * 0.5f;
                s->vel.y -= 80.0f;
                // Spin reverses direction
                s->angle_vel = -s->angle_vel * 1.2f;
                // Change ownership: now lethal to the original thrower
                s->owner      = target->id;
                s->rebounding = true;
                // Brief cooldown so it can't immediately re-hit the parrying player
                s->hit_cooldown = 6;
                // Parrying player gets their sword back (they deflected it with their blade)
                target->has_sword = true;
                // Visual: small stun on the parrying player's opponent (the thrower)
                Player *thrower = (target == p0) ? p1 : p0;
                if (thrower->state != STATE_DEAD) {
                    thrower->state      = STATE_STUNNED;
                    thrower->stun_timer = 10;
                    thrower->state_timer = 0;
                }
                break;
            }
            if (rect_overlap(sword_rect, target->hurtbox)) {
                player_kill(target);
                s->active = false;
                if (t == 0) result.p0_hit = HIT_BODY;
                else        result.p1_hit = HIT_BODY;
                break;
            }
        }
    }

    return result;
}

void combat_throw_sword(Player *p, ThrowingSword *sword) {
    sword->pos.x      = p->sword_tip.x;
    sword->pos.y      = p->sword_tip.y;
    sword->vel.x      = (float)p->facing * SWORD_THROW_SPEED;
    sword->vel.y      = p->body.vel.y * 0.5f - 50.0f;
    sword->angle      = 0.0f;
    sword->angle_vel  = (float)p->facing * 15.0f;
    sword->active     = true;
    sword->owner      = p->id;
    sword->rebounding  = false;
    sword->hit_cooldown = 0;
}

void combat_update_thrown_swords(ThrowingSword *swords, int num_swords,
                                  Player *p0, Player *p1, float dt) {
    Player *players[2] = {p0, p1};

    for (int i = 0; i < num_swords; i++) {
        ThrowingSword *s = &swords[i];
        if (!s->active) continue;

        s->vel.y += GRAVITY * dt;
        s->pos.x += s->vel.x * dt;
        s->pos.y += s->vel.y * dt;
        s->angle += s->angle_vel * dt;

        // Ground: sword sticks (stops spinning, slides to a halt)
        if (s->pos.y >= g_ground_y()) {
            s->pos.y    = g_ground_y();
            s->vel.y    = 0.0f;
            s->vel.x   *= 0.7f;
            s->angle_vel = s->vel.x * 0.05f;  // slow roll matching slide
            // Once nearly stopped, owner no longer matters — anyone can pick it up
            if (s->vel.x > -5.0f && s->vel.x < 5.0f) {
                s->vel.x    = 0.0f;
                s->angle_vel = 0.0f;
                s->rebounding = false;  // grounded and still — safe to pick up
            }
        }

        // Pickup: any player without a sword can grab it when on the ground and still
        // (while airborne/rebounding only the non-owner can be hit by it, handled in combat_resolve)
        if (!s->rebounding) {
            Rect pickup_rect;
            pickup_rect.x = s->pos.x - 12;
            pickup_rect.y = s->pos.y - 12;
            pickup_rect.w = 24;
            pickup_rect.h = 24;

            for (int j = 0; j < 2; j++) {
                Player *pl = players[j];
                if (pl->has_sword) continue;
                if (pl->state == STATE_DEAD) continue;
                if (rect_overlap(pickup_rect, pl->hurtbox)) {
                    pl->has_sword = true;
                    s->active     = false;
                    break;
                }
            }
        }

        // Arena wall rebound: match the ±2000 bounds used by physics_update.
        // Reverse horizontal velocity with a small energy loss, spin reverses
        // to match the new direction, and the sword is nudged back inside so it
        // doesn't tunnel through on the next frame.
        #define ARENA_BOUND 2000.0f
        #define WALL_RESTITUTION 0.75f   // fraction of speed kept after bounce
        if (s->pos.x < -ARENA_BOUND) {
            s->pos.x    = -ARENA_BOUND;
            s->vel.x    = fabsf(s->vel.x) * WALL_RESTITUTION;   // reflect rightward
            s->angle_vel = -s->angle_vel * WALL_RESTITUTION;
        } else if (s->pos.x > ARENA_BOUND) {
            s->pos.x    = ARENA_BOUND;
            s->vel.x    = -fabsf(s->vel.x) * WALL_RESTITUTION;  // reflect leftward
            s->angle_vel = -s->angle_vel * WALL_RESTITUTION;
        }
    }
}
