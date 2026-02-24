#ifndef AI_H
#define AI_H

#include "types.h"
#include "player.h"

// ---------------------------------------------------------------------------
// AI difficulty: HARD only (as requested — tough, clever opponent)
// ---------------------------------------------------------------------------

// Attack patterns the AI can execute. It sequences these together so the
// human can eventually read patterns, but there are enough combinations to
// stay unpredictable.
typedef enum AIPattern {
    PAT_NONE = 0,

    // --- Armed patterns ---
    PAT_PRESSURE,        // walk in, attack at close range
    PAT_FEINT_ATTACK,    // approach, pause (fake), then attack
    PAT_JUMP_ATTACK,     // jump in, aerial attack
    PAT_CROUCH_POKE,     // crouch-walk then attack low
    PAT_THROW_FOLLOWUP,  // throw sword then close in (risky, expects catch back)
    PAT_BAIT_PARRY,      // hold still inside attack range, wait for human to swing, parry counter
    PAT_SPACING_DANCE,   // walk back and forth to bait human, then punish
    PAT_RUSH_ATTACK,     // sprint and attack rapidly
    PAT_RETREAT_THROW,   // back up then throw sword

    // --- Unarmed patterns ---
    PAT_RETREAT_UNARMED, // run away from human, keep distance
    PAT_PLATFORM_FLEE,   // jump to platform to avoid human while unarmed
    PAT_SWORD_RETRIEVE,  // move toward nearest grounded sword
    PAT_CATCH_WAIT,      // hold position and wait to catch rebounding sword

    PAT_COUNT,
} AIPattern;

// Internal AI state machine
typedef enum AIState {
    AI_IDLE,
    AI_EXECUTE_PATTERN,
    AI_REACT_PARRY,       // mid-swing reaction: raise parry
    AI_REACT_CATCH,       // incoming sword: try to catch
    AI_RETRIEVE_SWORD,    // unarmed: path to sword on ground
    AI_FLEE,              // unarmed: avoid human
} AIState;

// Per-tick reaction memory: what the AI perceived this frame
typedef struct AIPerception {
    float  dist;           // signed horizontal distance to human (positive = human is right)
    float  abs_dist;       // |dist|
    float  human_vel_x;   // human horizontal velocity
    bool   human_attacking;// human is in STATE_ATTACK
    bool   human_parrying; // human is in STATE_PARRY
    bool   human_has_sword;
    bool   sword_in_air;   // any thrown sword currently active
    bool   sword_rebounding;         // rebounding thrown sword exists
    float  sword_x;        // x of nearest active thrown sword
    float  sword_y;
    float  sword_vel_x;    // velocity (to predict direction)
    int    sword_owner;    // owner id of that sword
    float  ground_sword_x; // x of nearest grounded (still) sword, FLT_MAX if none
    float  ground_sword_y;
    bool   ground_sword_exists;
} AIPerception;

#define AI_HISTORY_LEN 8   // remember last N human actions for pattern learning

typedef struct AIBrain {
    AIState    state;
    AIPattern  current_pattern;
    int        pattern_timer;       // frames left in current pattern execution
    int        pattern_phase;       // step within the pattern (0,1,2...)
    int        decision_cooldown;   // frames until next top-level decision

    // Reaction timers (simulates human-like reaction delay)
    int        reaction_delay;      // frames of lookahead before committing
    int        react_timer;         // countdown for current reaction

    // Combo / sequence memory
    AIPattern  pattern_queue[4];    // queued patterns to execute in order
    int        queue_len;
    int        queue_head;

    // Anti-repetition: track last 3 patterns so AI doesn't spam same one
    AIPattern  last_patterns[3];
    int        last_pattern_idx;

    // Aggression level: 0=cautious, 100=full aggression. Varies over time.
    float      aggression;          // 0..100
    int        aggression_timer;    // frames until next aggression shift

    // Timed inputs: the AI queues up button presses with delays
    int        jump_in;             // frames until jump press fires (0=no pending)
    int        attack_in;
    int        parry_in;
    int        throw_in;
    bool       hold_parry;          // hold parry this frame
    bool       hold_left;
    bool       hold_right;
    bool       hold_crouch;

    // Sword catch state
    bool       trying_to_catch;     // actively positioning to catch rebounding sword
    int        catch_timer;

    // Feint state
    bool       feinting;
    int        feint_timer;

    // Spacing state
    float      preferred_dist;      // target distance from human
    int        spacing_dir;         // +1 or -1

    // Memory of human attack timing (for parry prediction)
    int        human_attack_history[AI_HISTORY_LEN];  // frame numbers of human attacks
    int        human_attack_count;
    int        avg_attack_interval; // estimated frames between human attacks

    // Frame counter (local copy, for timing)
    uint32_t   frame;

    // Random seed for deterministic noise
    uint32_t   rng;
} AIBrain;

void ai_init(AIBrain *ai);

// Main entry point: call once per fixed update.
// Fills `out` with the AI's input for this frame.
// `ai_player` = player the AI controls (player[1])
// `human`     = the human player (player[0])
// `swords`    = array of thrown swords
// `num_swords`= MAX_THROWN_SWORDS
void ai_update(AIBrain *ai,
               const Player *ai_player,
               const Player *human,
               const ThrowingSword *swords, int num_swords,
               Input *out);

#endif
