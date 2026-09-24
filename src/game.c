/* SPDX-License-Identifier: MIT */
#include "game.h"
#include "render.h"
#include "audio.h"

#define GRAVITY      (0.5f * 60.0f * 60.0f)
#define JUMP_FORCE   (-10.0f * 60.0f)

#define BIRD_X        BIRD_X_POS
#define BIRD_W_F      ((float)BIRD_W)
#define BIRD_H_F      ((float)BIRD_H)
#define GROUND_H_F    ((float)GROUND_H)
#define SCREEN_H_F    1080.0f

#define MAX_RAMP_SPEED 12.0f
#define RAMP_STEP      0.25f
#define MIN_TOP_PIPE_VISIBLE 45.0f

const char *game_diff_name(enum diff d) {
    switch (d) {
    case DIFF_EASY:   return "EASY";
    case DIFF_NORMAL: return "NORMAL";
    case DIFF_HARD:   return "HARD";
    case DIFF_RACER:  return "RACER";
    default:          break;
    }
    return "?";
}

const char *game_screen_name(enum screen_mode m) {
    switch (m) {
    case SCREEN_FULL: return "FULL";
    case SCREEN_16_9: return "16:9";
    case SCREEN_4_3:  return "4:3";
    default:          break;
    }
    return "?";
}

float game_diff_pipe_speed(enum diff d) {
    switch (d) {
    case DIFF_EASY:   return 4.0f;
    case DIFF_NORMAL: return 5.0f;
    case DIFF_HARD:   return 7.0f;
    case DIFF_RACER:  return 5.0f;
    default:          break;
    }
    return 5.0f;
}

float game_diff_pipe_gap(enum diff d) {
    switch (d) {
    case DIFF_EASY:   return 380.0f;
    case DIFF_NORMAL: return 300.0f;
    case DIFF_HARD:   return 240.0f;
    case DIFF_RACER:  return 300.0f;
    default:          break;
    }
    return 300.0f;
}

static void release(struct game *g, struct pipe_pair *p) {
    p->active = 0; p->passed = 0; p->x = -9999.0f;
    for (int i = 0; i < g->active_count; i++) {
        if (g->active[i] == p) {
            g->active[i] = g->active[--g->active_count];
            return;
        }
    }
}

static struct pipe_pair *obtain(struct game *g) {
    for (int i = 0; i < POOL_PAIRS; i++)
        if (!g->pool[i].active) return &g->pool[i];
    return &g->pool[0];
}

static u32 s_rng = 0x13579BDFu;
static u32 rng_next(void) {
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 16;
}

static void spawn_pipe(struct game *g) {
    float min_gap_y = g->pipe_gap + MIN_TOP_PIPE_VISIBLE;
    float max_gap_y = SCREEN_H_F - GROUND_H_F - g->pipe_gap - 10.0f;
    if (max_gap_y < min_gap_y) max_gap_y = min_gap_y;

    float t  = (float)(rng_next() & 0x7FFF) / 32768.0f;
    float gy = min_gap_y + t * (max_gap_y - min_gap_y);

    struct pipe_pair *p = obtain(g);
    p->active  = 1;
    p->passed  = 0;
    p->x       = 1920.0f;
    p->y_top   = gy - g->pipe_gap;
    p->y_bot   = gy;
    g->active[g->active_count++] = p;
}

void game_init(struct game *g) {
    for (int i = 0; i < POOL_PAIRS; i++) {
        g->pool[i].active = 0;
        g->pool[i].x = -9999.0f;
    }
    g->active_count = 0;
    g->state = GS_MENU;
    g->screen_mode = SCREEN_FULL;
    g->menu_cursor = 0;
    g->show_credits = 0;
    g->vibration_on = 1;
    g->sfx_volume   = 100;
    game_set_diff(g, DIFF_NORMAL);
    g->is_night = 0;
    g->score = 0;
    g->last_score = 0;
    g->high_score = 0;
    g->lifetime_pipes = 0;
    s_rng = 0x13579BDFu ^ (u32)(g->lifetime_pipes + 1);
}

void game_set_diff(struct game *g, enum diff d) {
    g->diff = d;
    g->pipe_speed = game_diff_pipe_speed(d);
    g->pipe_gap   = game_diff_pipe_gap(d);
}

void game_reset_run(struct game *g) {
    g->bird_y = SCREEN_H_F / 2.0f;
    g->bird_vy = 0.0f;
    g->pipe_speed = game_diff_pipe_speed(g->diff);
    g->pipe_gap   = game_diff_pipe_gap(g->diff);
    g->pipe_spawn_acc = 0.0f;
    g->pipe_step  = 500.0f;
    g->score = 0;
    g->bg_scroll = 0.0f;
    g->base_scroll = 0.0f;
    for (int i = g->active_count - 1; i >= 0; i--)
        release(g, g->active[i]);
    g->active_count = 0;
    s_rng = 0x13579BDFu ^ (u32)(g->lifetime_pipes + 1);
}

void game_start(struct game *g) {
    game_reset_run(g);
    g->state = GS_READY;
}

void game_jump(struct game *g) {
    if (g->state == GS_READY) {
        g->state = GS_PLAYING;
        g->bird_vy = JUMP_FORCE;
        audio_play(A_SFX_JUMP, 0.6f);
    } else if (g->state == GS_PLAYING) {
        g->bird_vy = JUMP_FORCE;
        audio_play(A_SFX_JUMP, 0.6f);
    }
}

void game_over(struct game *g) {
    g->state = GS_GAMEOVER;
    if (g->score > g->last_score)  g->last_score = g->score;
    if (g->score > g->high_score)  g->high_score = g->score;
    audio_play(A_SFX_HIT, 0.8f);
}

static void update_pipes(struct game *g, float dt) {
    for (int i = g->active_count - 1; i >= 0; i--) {
        struct pipe_pair *p = g->active[i];
        p->x -= g->pipe_speed * 60.0f * dt;

        if (!p->passed && p->x + (float)PIPE_W < BIRD_X) {
            p->passed = 1;
            g->score++;
            g->lifetime_pipes++;
            if (g->diff == DIFF_RACER && g->pipe_speed < MAX_RAMP_SPEED)
                g->pipe_speed += RAMP_STEP;
            else if (g->diff == DIFF_HARD && g->pipe_speed < 10.0f)
                g->pipe_speed += RAMP_STEP * 0.5f;
            audio_play(A_SFX_SCORE, 0.5f);
        }

        if (p->x + (float)PIPE_W < -80.0f)
            release(g, p);
    }

    g->pipe_spawn_acc += g->pipe_speed * 60.0f * dt;
    if (g->pipe_spawn_acc >= g->pipe_step) {
        g->pipe_spawn_acc = 0.0f;
        spawn_pipe(g);
    }
}

static int aabb(float ax, float ay, float aw, float ah,
                float bx, float by, float bw, float bh) {
    return !(bx > ax + aw || bx + bw < ax || by > ay + ah || by + bh < ay);
}

void game_update(struct game *g, float dt) {
    if (g->state == GS_READY || g->state == GS_PLAYING) {
        if (g->state == GS_PLAYING) {
            g->bird_vy += GRAVITY * dt;
        } else {
            g->bird_vy = 0.0f;
            g->bird_y  = SCREEN_H_F / 2.0f;
        }
        g->bird_y += g->bird_vy * dt;

        if (g->bird_y < 0.0f) { g->bird_y = 0.0f; g->bird_vy = 0.0f; }
        if (g->bird_y + BIRD_H_F > SCREEN_H_F - GROUND_H_F) {
            g->bird_y = SCREEN_H_F - GROUND_H_F - BIRD_H_F;
            game_over(g);
            return;
        }
    }

    if (g->state == GS_PLAYING) {
        update_pipes(g, dt);

        float bx = BIRD_X, by = g->bird_y;
        for (int i = 0; i < g->active_count; i++) {
            struct pipe_pair *p = g->active[i];
            if (aabb(bx, by, BIRD_W_F, BIRD_H_F,
                     p->x, p->y_top - (float)PIPE_H, (float)PIPE_W, (float)PIPE_H)
             || aabb(bx, by, BIRD_W_F, BIRD_H_F,
                     p->x, p->y_bot, (float)PIPE_W, (float)PIPE_H)) {
                game_over(g);
                return;
            }
        }
    }

    g->bg_scroll   -= (g->pipe_speed / 3.0f) * 60.0f * dt;
    g->base_scroll -= g->pipe_speed * 60.0f * dt;

    if (g->bg_scroll   < -(float)BG_W)   g->bg_scroll   += (float)BG_W;
    if (g->base_scroll < -(float)BASE_W) g->base_scroll += (float)BASE_W;
}
