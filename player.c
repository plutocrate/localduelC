#include "player.h"
#include <math.h>
#include <string.h>

static void compute_sword(Player *p);

void player_init(Player *p, int id, float x) {
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->facing = (id == 0) ? 1 : -1;
    p->has_sword = true;
    p->score = 0;
    physics_init_body(&p->body, x, g_ground_y() - PLAYER_HEIGHT, PLAYER_W, PLAYER_HEIGHT);
    p->state = STATE_IDLE;
    compute_sword(p);
}

static void compute_sword(Player *p) {
    // Sword base starts at player center chest
    float cx = p->body.pos.x + p->body.size.x * 0.5f;
    float cy = p->body.pos.y + p->body.size.y * 0.35f;

    // Sword angle depends on state
    float angle = 0.0f;
    if (p->state == STATE_ATTACK) {
        // Thrust forward - angle progresses then retracts
        float t = (float)p->attack_frame / ATTACK_FRAMES;
        angle = (t < 0.5f) ? (t * 2.0f * 0.3f) : ((1.0f - t) * 2.0f * 0.3f);
        angle *= (float)p->facing; // slight vertical component
    } else if (p->state == STATE_PARRY) {
        angle = -(float)p->facing * 0.6f; // held up for parry
    } else if (p->crouching) {
        angle = (float)p->facing * 0.4f;
    }

    p->sword_angle = angle;

    float base_x = cx + p->facing * 4.0f;
    float base_y = cy;
    float tip_x = base_x + p->facing * cosf(angle) * SWORD_LENGTH;
    float tip_y = base_y + sinf(angle) * SWORD_LENGTH;

    p->sword_base.x = base_x;
    p->sword_base.y = base_y;
    p->sword_tip.x  = tip_x;
    p->sword_tip.y  = tip_y;
}

void player_compute_boxes(Player *p) {
    float bx = p->body.pos.x;
    float by = p->body.pos.y;
    float bw = p->body.size.x;
    float bh = p->body.size.y;

    // Hurtbox = body
    p->hurtbox.x = bx + 2;
    p->hurtbox.y = by + 2;
    p->hurtbox.w = bw - 4;
    p->hurtbox.h = bh - 4;

    // Weapon hitbox: active rectangle around sword tip during attack
    if (p->state == STATE_ATTACK && p->attack_frame >= 2 && p->attack_frame <= 9) {
        float tx = p->sword_tip.x;
        float ty = p->sword_tip.y;
        p->weapon_hitbox.x = tx - 8;
        p->weapon_hitbox.y = ty - 8;
        p->weapon_hitbox.w = 16;
        p->weapon_hitbox.h = 16;
    } else {
        p->weapon_hitbox.x = 0;
        p->weapon_hitbox.y = 0;
        p->weapon_hitbox.w = 0;
        p->weapon_hitbox.h = 0;
    }

    // Parry box: zone in front of player during active parry frames
    int parry_active = (p->state == STATE_PARRY &&
                        p->state_timer >= (PARRY_FRAMES - PARRY_ACTIVE_END) &&
                        p->state_timer <= (PARRY_FRAMES - PARRY_ACTIVE_START));
    if (parry_active) {
        float px2 = bx + (p->facing > 0 ? bw : -32.0f);
        float py2 = by + bh * 0.05f;
        p->parry_box.x = px2;
        p->parry_box.y = py2;
        p->parry_box.w = 32.0f;
        p->parry_box.h = bh * 0.90f;
    } else {
        p->parry_box.x = 0;
        p->parry_box.y = 0;
        p->parry_box.w = 0;
        p->parry_box.h = 0;
    }
}

void player_update(Player *p, const Input *in, float dt,
                   const Platform *plats, int num_plats) {
    if (p->state == STATE_DEAD) {
        player_update_ragdoll(p, dt);
        if (p->respawn_timer > 0) p->respawn_timer--;
        return;
    }

    bool can_act = (p->state != STATE_STUNNED &&
                    p->state != STATE_ATTACK &&
                    p->state != STATE_PARRY &&
                    p->state != STATE_THROW);

    // Movement
    if (can_act) {
        if (in->left  && !in->right) physics_apply_walk(&p->body, -1.0f, dt);
        if (in->right && !in->left)  physics_apply_walk(&p->body,  1.0f, dt);

        // Update facing
        if (in->left  && !in->right) p->facing = -1;
        if (in->right && !in->left)  p->facing =  1;

        if (in->jump) physics_apply_jump(&p->body);

        // S/Down only drops through one-way platforms — it does NOT crouch
        p->crouching = false;
        p->body.size.y = PLAYER_HEIGHT;

        // Drop through one-way platforms: press down while standing on one
        if (in->crouch && p->body.on_ground && !p->on_main_ground) {
            p->body.drop_through = true;
            p->body.on_ground    = false;
            p->body.pos.y       += 2.0f;  // nudge below surface so we don't immediately re-land
        }
    }

    // State transitions
    if (can_act && in->attack && p->has_sword) {
        p->state = STATE_ATTACK;
        p->state_timer = ATTACK_FRAMES;
        p->attack_frame = 0;
    } else if (can_act && in->parry) {
        p->state = STATE_PARRY;
        p->state_timer = PARRY_FRAMES;
    } else if (can_act && in->throw_weapon && p->has_sword) {
        p->state = STATE_THROW;
        p->state_timer = 8;
        p->has_sword = false;
    }

    // State timers
    if (p->state_timer > 0) {
        p->state_timer--;
        if (p->state == STATE_ATTACK) {
            p->attack_frame = ATTACK_FRAMES - p->state_timer;
        }
        if (p->state_timer == 0) {
            p->state = STATE_IDLE;
            p->attack_frame = 0;
        }
    } else {
        // Derive display state from movement
        if (p->state != STATE_ATTACK && p->state != STATE_PARRY &&
            p->state != STATE_STUNNED && p->state != STATE_THROW) {
            if (!p->body.on_ground) {
                p->state = STATE_JUMP;
            } else if (fabsf(p->body.vel.x) > 10.0f) {
                p->state = STATE_WALK;
            } else {
                p->state = STATE_IDLE;
            }
        }
    }

    // Stun
    if (p->state == STATE_STUNNED) {
        if (p->stun_timer > 0) {
            p->stun_timer--;
        } else {
            p->state = STATE_IDLE;
        }
    }

    // Record foot position before physics integrates
    float prev_bottom = p->body.pos.y + p->body.size.y;

    physics_update(&p->body, dt, g_ground_y());

    // Track whether player landed on the main ground (not a platform)
    p->on_main_ground = (p->body.on_ground);

    // Resolve one-way platforms (overrides on_ground if landing on one)
    if (!p->body.drop_through) {
        bool on_plat = physics_resolve_platforms(&p->body, prev_bottom, plats, num_plats);
        if (on_plat) {
            p->on_main_ground = false;  // standing on platform, not main ground
        }
    }

    // Apply friction AFTER platform resolution so on_ground is correct for both
    // the main floor and one-way platforms. This is the fix for platform sliding.
    physics_apply_friction(&p->body);

    // Clear drop_through after one frame
    p->body.drop_through = false;

    compute_sword(p);
    player_compute_boxes(p);
}

Rect player_sword_rect(const Player *p) {
    return p->weapon_hitbox;
}

Rect player_parry_rect(const Player *p) {
    return p->parry_box;
}

void player_kill(Player *p) {
    p->state = STATE_DEAD;
    p->respawn_timer = 120; // 2 seconds

    // Activate ragdoll
    p->ragdoll.active = true;
    p->ragdoll.timer = 90;
    // Initialize ragdoll bones from current position
    float cx = p->body.pos.x + p->body.size.x * 0.5f;
    float cy = p->body.pos.y;

    // Head
    p->ragdoll.bones[0].pos = (Vec2){cx, cy};
    p->ragdoll.bones[0].vel = (Vec2){p->body.vel.x * 0.5f + (float)(p->facing * -100), -200.0f};
    // Torso
    p->ragdoll.bones[1].pos = (Vec2){cx, cy + 20};
    p->ragdoll.bones[1].vel = (Vec2){p->body.vel.x * 0.5f, -100.0f};
    // Arms
    p->ragdoll.bones[2].pos = (Vec2){cx - 10, cy + 20};
    p->ragdoll.bones[2].vel = (Vec2){-150.0f, -150.0f};
    p->ragdoll.bones[3].pos = (Vec2){cx + 10, cy + 20};
    p->ragdoll.bones[3].vel = (Vec2){ 150.0f, -150.0f};
    // Legs
    p->ragdoll.bones[4].pos = (Vec2){cx, cy + 45};
    p->ragdoll.bones[4].vel = (Vec2){p->body.vel.x, 50.0f};
}

void player_respawn(Player *p, float x, int facing) {
    float cur_y = p->body.pos.y;  // keep existing y reference
    physics_init_body(&p->body, x, g_ground_y() - PLAYER_HEIGHT, PLAYER_W, PLAYER_HEIGHT);
    (void)cur_y;
    p->facing = facing;
    p->state = STATE_IDLE;
    p->state_timer = 0;
    p->attack_frame = 0;
    p->stun_timer = 0;
    p->has_sword = true;
    p->crouching = false;
    p->ragdoll.active = false;
}

void player_update_ragdoll(Player *p, float dt) {
    if (!p->ragdoll.active) return;
    for (int i = 0; i < RAGDOLL_BONES; i++) {
        RagdollBone *b = &p->ragdoll.bones[i];
        b->vel.y += GRAVITY * dt;
        b->pos.x += b->vel.x * dt;
        b->pos.y += b->vel.y * dt;
        // simple ground
        if (b->pos.y > g_ground_y()) {
            b->pos.y = g_ground_y();
            b->vel.y *= -0.3f;
            b->vel.x *= 0.7f;
        }
        b->angle += b->vel.x * dt * 0.05f;
    }
    if (p->ragdoll.timer > 0) p->ragdoll.timer--;
}

void player_to_sync(const Player *p, PlayerSync *s, uint32_t frame) {
    s->px = p->body.pos.x;
    s->py = p->body.pos.y;
    s->vx = p->body.vel.x;
    s->vy = p->body.vel.y;
    s->state = (uint8_t)p->state;
    s->facing = (uint8_t)(p->facing + 1);  // encode -1/+1 as 0/2
    s->has_sword = p->has_sword ? 1 : 0;
    s->attack_frame = (uint8_t)p->attack_frame;
    s->stun_timer = (uint8_t)(p->stun_timer > 255 ? 255 : p->stun_timer);
    s->frame = frame;
}

void player_from_sync(Player *p, const PlayerSync *s) {
    p->body.pos.x = s->px;
    p->body.pos.y = s->py;
    p->body.vel.x = s->vx;
    p->body.vel.y = s->vy;
    p->state = (PlayerState)s->state;
    p->facing = (int)s->facing - 1;
    p->has_sword = s->has_sword != 0;
    p->attack_frame = s->attack_frame;
    p->stun_timer = s->stun_timer;
    player_compute_boxes(p);
}
