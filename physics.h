#ifndef PHYSICS_H
#define PHYSICS_H

#include "types.h"

typedef struct PhysicsBody {
    Vec2  pos;       // top-left of bounding box
    Vec2  vel;
    Vec2  size;
    bool  on_ground;
    bool  was_on_ground;
    bool  drop_through;   // set for one frame to fall through one-way platforms
} PhysicsBody;

// One-way platform: solid only when landing on top (climbable from below)
typedef struct Platform {
    float x;   // left edge (world space)
    float y;   // top surface Y (world space)
    float w;   // width
} Platform;

#define MAX_PLATFORMS 8

// ground_y is passed in so physics stays decoupled from screen state
void physics_init_body(PhysicsBody *b, float x, float y, float w, float h);
void physics_update(PhysicsBody *b, float dt, float ground_y);
void physics_apply_jump(PhysicsBody *b);
void physics_apply_walk(PhysicsBody *b, float dir, float dt);

// Resolve one-way platform collisions after physics_update.
// Returns true if the body is standing on any platform this frame.
void physics_apply_friction(PhysicsBody *b);
bool physics_resolve_platforms(PhysicsBody *b, float prev_bottom,
                               const Platform *plats, int num_plats);

// AABB helpers
bool rect_overlap(Rect a, Rect b);
Rect body_to_rect(const PhysicsBody *b);
Rect rect_from_center(float cx, float cy, float w, float h);

#endif
