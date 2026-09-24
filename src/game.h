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

/* ---- Reference-matched dimensions (matches PsVue JS port) ---- */
#define GAME_SCALE_NUM 1080
#define GAME_SCALE_DEN 512
#define GAME_SCALE_F   (1080.0f / 512.0f)
#define GAME_SCALE_FP  540                       /* 2.109375 * 256 */

#define BG_W_ORIG   288
#define BG_H_ORIG   512
#define BG_W  ((BG_W_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 608 */
#define BG_H  ((BG_H_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 1080 */

#define BASE_W_ORIG 336
#define BASE_H_ORIG 112
#define BASE_W ((BASE_W_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 709 */
#define BASE_H ((BASE_H_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 236 */
#define GROUND_H BASE_H

#define PIPE_W_ORIG 52
#define PIPE_H_ORIG 320
#define PIPE_W ((PIPE_W_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 110 */
#define PIPE_H ((PIPE_H_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 675 */

#define BIRD_W_ORIG 34
#define BIRD_H_ORIG 24
#define BIRD_W ((BIRD_W_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 72 */
#define BIRD_H ((BIRD_H_ORIG * GAME_SCALE_NUM + GAME_SCALE_DEN/2) / GAME_SCALE_DEN)  /* 51 */

#define GAMEOVER_W ((192 * 3 + 1) / 2)   /* 288 */
#define GAMEOVER_H ((42  * 3 + 1) / 2)   /*  63 */

#define BIRD_X_POS 300.0f

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
