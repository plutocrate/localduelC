#ifndef PLAYER_H_GUARD
#define PLAYER_H_GUARD

#include "types.h"
#include "physics.h"
#include "input.h"

#define SWORD_SEGMENTS 3

typedef struct ThrowingSword {
    Vec2  pos;
    Vec2  vel;
    float angle;
    float angle_vel;
    bool  active;
    int   owner;        // which player threw it (changes on parry rebound)
    bool  rebounding;   // true after being parried — now lethal to original thrower
    int   hit_cooldown; // frames of immunity after parry to prevent instant double-hit
} ThrowingSword;

typedef struct RagdollBone {
    Vec2  pos;
    Vec2  vel;
    float angle;
} RagdollBone;

#define RAGDOLL_BONES 5
typedef struct Ragdoll {
    RagdollBone bones[RAGDOLL_BONES]; // head, torso, l_arm, r_arm, legs
    bool active;
    int  timer; // frames until cleaned up
} Ragdoll;

typedef struct Player {
    PhysicsBody body;
    Vec2        sword_tip;     // world position of sword tip (computed)
    Vec2        sword_base;    // world position of sword base

    int         facing;        // +1 right, -1 left
    PlayerState state;
    int         state_timer;   // frames remaining in state
    bool        has_sword;
    bool        crouching;
    bool        on_main_ground;  // true when standing on the floor, false when on a platform

    // hitbox / hurtbox
    Rect        hurtbox;       // body hurtbox
    Rect        weapon_hitbox; // active during attack frames
    Rect        parry_box;     // active during parry window

    // combat
    int         attack_frame;
    int         stun_timer;
    int         respawn_timer;

    // scoring
    int         score;         // number of kills

    // visual
    Ragdoll     ragdoll;
    float       sword_angle;   // display angle of sword (procedural)

    int         id;            // 0 or 1
} Player;

void player_init(Player *p, int id, float x);
void player_update(Player *p, const Input *in, float dt,
                   const Platform *plats, int num_plats);
void player_compute_boxes(Player *p);

// Returns sword hitbox rect in world space (only valid during attack)
Rect player_sword_rect(const Player *p);
Rect player_parry_rect(const Player *p);

void player_kill(Player *p);
void player_respawn(Player *p, float x, int facing);

void player_update_ragdoll(Player *p, float dt);

// Sync packet (for network)
typedef struct PlayerSync {
    float px, py;
    float vx, vy;
    uint8_t state;
    uint8_t facing;
    uint8_t has_sword;
    uint8_t attack_frame;
    uint8_t stun_timer;
    uint32_t frame;
} PlayerSync;

void player_to_sync(const Player *p, PlayerSync *s, uint32_t frame);
void player_from_sync(Player *p, const PlayerSync *s);

#endif
