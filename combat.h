#ifndef COMBAT_H
#define COMBAT_H

#include "types.h"
#include "player.h"

typedef struct CombatResult {
    HitResult p0_hit;   // what happened to player 0
    HitResult p1_hit;   // what happened to player 1
    bool      sword_clash;
} CombatResult;

// Check all combat interactions for this frame and apply effects
CombatResult combat_resolve(Player *p0, Player *p1, ThrowingSword *swords, int num_swords);

// Sword throw helper
void combat_throw_sword(Player *p, ThrowingSword *sword);
void combat_update_thrown_swords(ThrowingSword *swords, int num_swords, Player *p0, Player *p1, float dt);

#endif
