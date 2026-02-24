#ifndef INPUT_H
#define INPUT_H

#include "types.h"

// Player 1: WASD + J (attack) + K (parry) + L (throw)
// Player 2 (local only): Arrow keys + numpad
// In multiplayer, both use same layout (P1 keys) since each is on own machine

// InputBuffer latches edge-triggered buttons (attack/parry/jump/throw) so they
// survive across multiple fixed-update calls within a single render frame.
// Call input_buffer_poll() once per render frame, then input_buffer_consume()
// once per fixed update to get the Input for that tick.
typedef struct InputBuffer {
    // Held states (safe to read any time)
    bool left;
    bool right;
    bool crouch;
    // Latched edge states - set on press, cleared after one consume()
    bool jump;
    bool attack;
    bool parry;
    bool throw_weapon;
} InputBuffer;

void input_init(void);

// Call ONCE per real render frame to sample Raylib key state into the buffer.
void input_buffer_poll_p1(InputBuffer *buf);
void input_buffer_poll_p2(InputBuffer *buf);

// Call ONCE per fixed update. Fills `in`, then clears latched buttons from buf.
// frame is the current fixed-update frame number.
void input_buffer_consume(InputBuffer *buf, Input *in, uint32_t frame);

// Legacy one-shot gather (still used by network path which calls per fixed update)
Input input_gather_p1(uint32_t frame);
Input input_gather_p2(uint32_t frame);  // local only

// Serialization for network
void   input_serialize(const Input *in, uint8_t *buf, int *len);
bool   input_deserialize(Input *in, const uint8_t *buf, int len);

#endif
