#ifndef GAME_H
#define GAME_H
#include "core.h"

enum diff {
    DIFF_EASY = 0,
    DIFF_NORMAL,
    DIFF_HARD,
    DIFF_RACER,
    DIFF_COUNT
};

enum gstate {
    GS_MENU = 0,
    GS_READY,
    GS_PLAYING,
    GS_PAUSED,
    GS_GAMEOVER,
};

struct pipe_pair {
    float x;
    float y_top, y_bot;
    u8    active, passed;
};

#define POOL_PAIRS 6

struct game {
    enum gstate state;
    enum diff   diff;

    /* physics */
    float bird_y, bird_vy;
    float pipe_speed;
    float pipe_gap;
    float pipe_spawn_acc;
    float pipe_step;

    /* scoring */
    int   score, last_score, high_score;
    u32   lifetime_pipes;

    /* background */
    u8    is_night;
    float bg_scroll, base_scroll;

    /* pipe pool */
    struct pipe_pair pool[POOL_PAIRS];
    struct pipe_pair *active[POOL_PAIRS];
    int   active_count;

    /* menu */
    int   menu_cursor;
    u8    vibration_on;
    u8    show_credits;

    u32   flash_until_ms;
};

void   game_init(struct game *g);
void   game_reset_run(struct game *g);
void   game_start(struct game *g);
void   game_update(struct game *g, float dt);
void   game_jump(struct game *g);
void   game_over(struct game *g);
void   game_set_diff(struct game *g, enum diff d);
float  game_diff_pipe_speed(enum diff d);
float  game_diff_pipe_gap(enum diff d);
const char *game_diff_name(enum diff d);

#endif
