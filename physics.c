#include "physics.h"
#include <math.h>

void physics_init_body(PhysicsBody *b, float x, float y, float w, float h) {
    b->pos.x = x;
    b->pos.y = y;
    b->vel.x = 0.0f;
    b->vel.y = 0.0f;
    b->size.x = w;
    b->size.y = h;
    b->on_ground = false;
    b->was_on_ground = false;
}

void physics_update(PhysicsBody *b, float dt, float ground_y) {
    b->was_on_ground = b->on_ground;

    // Apply gravity
    b->vel.y += GRAVITY * dt;

    // Integrate position
    b->pos.x += b->vel.x * dt;
    b->pos.y += b->vel.y * dt;

    // Ground collision
    float bottom = b->pos.y + b->size.y;
    if (bottom >= ground_y) {
        b->pos.y = ground_y - b->size.y;
        b->vel.y = 0.0f;
        b->on_ground = true;
    } else {
        b->on_ground = false;
    }

    // NOTE: friction is NOT applied here — it is applied after platform
    // resolution in physics_apply_friction() so platforms get ground friction.

    // Clamp tiny velocities to zero
    if (b->vel.x > -0.5f && b->vel.x < 0.5f) b->vel.x = 0.0f;

    // Arena bounds: fixed world space, independent of screen size
    if (b->pos.x < -2000.0f) b->pos.x = -2000.0f;
    if (b->pos.x > 2000.0f)  b->pos.x =  2000.0f;
}

void physics_apply_jump(PhysicsBody *b) {
    if (b->on_ground) {
        b->vel.y = JUMP_VEL;
        b->on_ground = false;
    }
}

void physics_apply_walk(PhysicsBody *b, float dir, float dt) {
    // dir: -1 left, +1 right
    b->vel.x += dir * RUN_ACCEL * dt;
    // clamp horizontal speed
    if (b->vel.x >  WALK_SPEED) b->vel.x =  WALK_SPEED;
    if (b->vel.x < -WALK_SPEED) b->vel.x = -WALK_SPEED;
}

bool physics_resolve_platforms(PhysicsBody *b, float prev_bottom,
                               const Platform *plats, int num_plats) {
    if (b->drop_through) return false;

    float cur_bottom = b->pos.y + b->size.y;
    bool landed = false;

    for (int i = 0; i < num_plats; i++) {
        const Platform *p = &plats[i];
        // Horizontal overlap check
        if (b->pos.x + b->size.x <= p->x || b->pos.x >= p->x + p->w) continue;
        // One-way: only collide when falling downward AND feet crossed the surface this frame
        if (b->vel.y < 0.0f) continue;                // moving up — pass through
        if (prev_bottom > p->y + 2.0f) continue;      // was already below surface — pass through
        if (cur_bottom < p->y) continue;               // haven't reached surface yet
        // Land on top
        b->pos.y = p->y - b->size.y;
        b->vel.y = 0.0f;
        b->on_ground = true;
        landed = true;
        break;  // only one platform at a time
    }
    return landed;
}

void physics_apply_friction(PhysicsBody *b) {
    float friction = b->on_ground ? FRICTION_GROUND : FRICTION_AIR;
    b->vel.x *= friction;
    if (b->vel.x > -0.5f && b->vel.x < 0.5f) b->vel.x = 0.0f;
}

bool rect_overlap(Rect a, Rect b) {
    return (a.x < b.x + b.w) && (a.x + a.w > b.x) &&
           (a.y < b.y + b.h) && (a.y + a.h > b.y);
}

Rect body_to_rect(const PhysicsBody *b) {
    Rect r;
    r.x = b->pos.x;
    r.y = b->pos.y;
    r.w = b->size.x;
    r.h = b->size.y;
    return r;
}

Rect rect_from_center(float cx, float cy, float w, float h) {
    Rect r;
    r.x = cx - w * 0.5f;
    r.y = cy - h * 0.5f;
    r.w = w;
    r.h = h;
    return r;
}
