#ifndef SAVE_H
#define SAVE_H
#include "core.h"
#include "game.h"

#define SAVE_PATH "/savedata0/.savegame/flappy.sav"
#define SAVE_VERSION 2

struct save_blob {
    u32 magic;
    u32 version;
    u32 high_score;
    u32 last_score;
    u32 lifetime_pipes;
    u32 diff;
    u32 is_night;
    u32 vibration_on;
    u32 checksum;
};

int  save_init(void);
int  save_available(void);
int  save_load(struct game *g);
int  save_write(const struct game *g);

#endif
