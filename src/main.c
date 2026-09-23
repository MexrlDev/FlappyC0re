#include "core.h"
#include "ps_libc.h"
#include "assets.h"
#include "render.h"
#include "game.h"
#include "audio.h"
#include "save.h"

/* ps_libc needs its own declarations we didn't duplicate */
void  ps_libc_init(void*, void*, void*, void*, void*, void*, void*, void*,
                   void*, void*, s32, u8*);

/* -------- platform state (persist so savedata keeps working after BSS wipe) */

struct ext_args {
    u64 eboot;      /* 0x00 */
    u64 r0;         /* 0x08 */
    u32 frame;      /* 0x10 */
    u32 pad0;       /* 0x14 */
    s32 log_fd;     /* 0x14 actually 0x18 -- aligned by 8? keep it simple */
    /* NOTE: the Lua writes at fixed offsets; layout below must match */

    /* we redefine cleanly: */
    u64 _pad_align;
    u64 handoff[8];
};

/* Simpler: define the layout the Lua actually uses */
struct ext_args_lua {
    u64 eboot;            /* 0x00 */
    u64 r0;               /* 0x08 */
    u32 frame;            /* 0x10 */
    u32 pad;              /* 0x14 */
    s32 log_fd;           /* 0x18 */
    s32 pad2;             /* 0x1C */
    u8  log_sa[16];       /* 0x20 */
    u64 tcp_srv;          /* 0x30 - WAD TCP listener */
    u64 wad_port;         /* 0x38 */
    u64 user_id;          /* 0x40 */
};

PERSIST static void *G, *D;

PERSIST static s32 video_h = -1;
PERSIST static void *vid_flip, *vid_open, *vid_close, *vid_reg, *vid_rate, *vid_evt;
PERSIST static u64 eq;
PERSIST static void *wait_eq, *create_eq, *delete_eq;
PERSIST static u8 *fbs_mem;
PERSIST static u64 start_us;
PERSIST static void *get_proc_time;
PERSIST static u64 total_frames;

PERSIST static s32 pad_h = -1;
PERSIST static void *pad_read_fn;
PERSIST static u8 pad_buf[128];
PERSIST static u32 pad_prev;

PERSIST static struct game game;

/* --------------------------------------------------------------------- */

static u32 now_ms(void) {
    if (!get_proc_time) return (u32)(total_frames * 1000 / 60);
    u64 t = NC(G, get_proc_time, 0,0,0,0,0,0);
    return (u32)((t - start_us) / 1000);
}

static void sleep_ms(u32 ms) {
    static void *usleep;
    if (!usleep) usleep = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    if (usleep) NC(G, usleep, (u64)ms * 1000, 0,0,0,0,0);
}

#define DS_CROSS    0x00004000u
#define DS_CIRCLE   0x00002000u
#define DS_TRIANGLE 0x00001000u
#define DS_SQUARE   0x00008000u
#define DS_UP       0x00000010u
#define DS_DOWN     0x00000040u
#define DS_LEFT     0x00000080u
#define DS_RIGHT    0x00000020u
#define DS_OPTIONS  0x00000008u

static u32 read_pad(void) {
    if (pad_h < 0 || !pad_read_fn) return 0;
    for (int i = 0; i < 128; i++) pad_buf[i] = 0;
    s32 r = (s32)NC(G, pad_read_fn, (u64)pad_h, (u64)pad_buf, 1, 0, 0, 0);
    if (r <= 0) return 0;
    u32 raw = *(u32*)pad_buf;
    if (raw & 0x80000000u) return 0;
    return raw & 0x001FFFFFu;
}

static u32 pad_pressed(void) {
    u32 cur = read_pad();
    u32 p = cur & ~pad_prev;
    pad_prev = cur;
    return p;
}

static void present(void) {
    NC(G, vid_flip, (u64)video_h, (u64)(total_frames & 1), 1,
       (u64)total_frames, 0, 0);
    if (eq && wait_eq) {
        u8 evt[64]; s32 n = 0;
        NC(G, wait_eq, eq, (u64)evt, 1, (u64)&n, 0, 0);
    } else {
        sleep_ms(16);
    }
    total_frames++;
}

static void render_swap_cb(void) {
    render_swap();
}

/* --------------------------------------------------------------------- */

static int video_init(u64 eboot) {
    void *cancel = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    if (cancel) {
        u64 gs = *(u64*)(eboot + EBOOT_GS_THREAD);
        if (gs) NC(G, cancel, gs, 0,0,0,0,0);
    }
    sleep_ms(300);

    s32 vmod = (s32)NC(G, SYM(G,D,LIBKERNEL_HANDLE,"sceKernelLoadStartModule"),
                       (u64)"libSceVideoOut.sprx", 0,0,0,0,0);
    vid_open  = SYM(G, D, vmod, "sceVideoOutOpen");
    vid_close = SYM(G, D, vmod, "sceVideoOutClose");
    vid_reg   = SYM(G, D, vmod, "sceVideoOutRegisterBuffers");
    vid_flip  = SYM(G, D, vmod, "sceVideoOutSubmitFlip");
    vid_rate  = SYM(G, D, vmod, "sceVideoOutSetFlipRate");
    vid_evt   = SYM(G, D, vmod, "sceVideoOutAddFlipEvent");
    if (!vid_open) return -1;

    s32 emu_vid = *(s32*)(eboot + EBOOT_VIDOUT);
    if (vid_close && emu_vid >= 0) NC(G, vid_close, (u64)emu_vid, 0,0,0,0,0);
    sleep_ms(100);

    video_h = (s32)NC(G, vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (video_h < 0) return -2;

    void *alloc_dm = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    void *map_dm   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    void *dm_size  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    create_eq      = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    wait_eq        = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    delete_eq      = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");

    if (create_eq) NC(G, create_eq, (u64)&eq, (u64)"flapQ", 0,0,0,0);
    if (vid_evt && eq) NC(G, vid_evt, eq, (u64)video_h, 0,0,0,0);

    u64 total = dm_size ? NC(G, dm_size, 0,0,0,0,0,0) : 0x300000000ULL;
    u64 phys = 0;
    NC(G, alloc_dm, 0, total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    void *vmem = 0;
    NC(G, map_dm, (u64)&vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!vmem) return -3;
    fbs_mem = vmem;

    u8 attr[64] = {0};
    *(u32*)(attr+0)  = 0x80000000;
    *(u32*)(attr+4)  = 1;
    *(u32*)(attr+12) = SCR_W;
    *(u32*)(attr+16) = SCR_H;
    *(u32*)(attr+20) = SCR_W;
    void *rbs[2] = { fbs_mem, fbs_mem + FB_ALIGNED };
    if (NC(G, vid_reg, (u64)video_h, 0, (u64)rbs, 2, (u64)attr, 0) != 0)
        return -4;
    if (vid_rate) NC(G, vid_rate, (u64)video_h, 0, 0,0,0,0);

    render_init((u32*)rbs[0], (u32*)rbs[1]);
    return 0;
}

static void audio_init_from(void) {
    s32 amod = (s32)NC(G, SYM(G,D,LIBKERNEL_HANDLE,"sceKernelLoadStartModule"),
                       (u64)"libSceAudioOut.sprx", 0,0,0,0,0);
    if (amod < 0) return;
    void *a_open  = SYM(G, D, amod, "sceAudioOutOpen");
    void *a_out   = SYM(G, D, amod, "sceAudioOutOutput");
    if (!a_open || !a_out) return;
    s32 h = (s32)NC(G, a_open, 0xFF, 0, 0, 2048, SAMPLE_RATE, AUDIO_S16_STEREO);
    if (h < 0) h = (s32)NC(G, a_open, 0xFF, 0, 0, 512, SAMPLE_RATE, AUDIO_S16_STEREO);
    if (h < 0) h = (s32)NC(G, a_open, 0xFF, 0, 0, 256, SAMPLE_RATE, AUDIO_S16_STEREO);
    audio_init(h, a_out, G);
}

static void pad_init_from(void) {
    s32 pmod = (s32)NC(G, SYM(G,D,LIBKERNEL_HANDLE,"sceKernelLoadStartModule"),
                       (u64)"libScePad.sprx", 0,0,0,0,0);
    if (pmod < 0) return;
    void *p_init = SYM(G, D, pmod, "scePadInit");
    void *p_geth = SYM(G, D, pmod, "scePadGetHandle");
    pad_read_fn  = SYM(G, D, pmod, "scePadRead");
    if (p_init) NC(G, p_init, 0,0,0,0,0,0);
    if (p_geth) pad_h = (s32)NC(G, p_geth, 1, 0, 0, 0, 0, 0);
}

/* --------------------------------------------------------------------- */
/*                            MENU UI                                    */

static const char *menu_items[] = {
    "START GAME",
    "DIFFICULTY",
    "BACKGROUND",
    "RESET SCORE",
    "SAVE STATUS",
    "EXIT",
};
#define MENU_COUNT 6

static void draw_menu(void) {
    render_clear(0xFF0A0A0A);

    /* two-tone title with shadow */
    render_text_center(80,  "FLAPPY BIRD", 0xFF000000u, 10);
    render_text_center(74,  "FLAPPY BIRD", 0xFFFFC030u, 10);
    render_text_center(240, "PS5 PORT",     0xFF808080u, 4);

    int base_y = 380;
    for (int i = 0; i < MENU_COUNT; i++) {
        char line[64];
        u32 col = (i == game.menu_cursor) ? 0xFFFFC030u : 0xFFD0D0D0u;
        const char *text = menu_items[i];
        if (i == 1) {
            snprintf(line, sizeof(line), "DIFFICULTY: %s", game_diff_name(game.diff));
            text = line;
        } else if (i == 2) {
            snprintf(line, sizeof(line), "BACKGROUND: %s", game.is_night ? "NIGHT" : "DAY");
            text = line;
        } else if (i == 3) {
            snprintf(line, sizeof(line), "RESET SCORE (%d)", game.high_score);
            text = line;
        } else if (i == 4) {
            snprintf(line, sizeof(line), "SAVE: %s",
                     save_available() ? "OK" : "NO SAVEDATA");
            text = line;
        }
        if (i == game.menu_cursor) {
            render_text(160, base_y + i * 70 - 6, ">", col, 4);
        }
        render_text(240, base_y + i * 70, text, col, 4);
    }

    /* HUD */
    render_text(40, 940, "X: SELECT   O: BACK", 0xFF808080u, 3);
    render_text(SCR_W - 40 - render_text_width("EGYDEVTEAM", 3),
                940, "EGYDEVTEAM", 0xFF606060u, 3);

    char hi[64];
    snprintf(hi, sizeof(hi), "HIGH: %d   LIFETIME: %u", game.high_score, game.lifetime_pipes);
    render_text(40, 990, hi, 0xFF404040u, 3);
}

static void draw_ready(void) {
    render_clear(0xFF000000);
    render_blit_scaled(A_BG_DAY, 0, 0, 6.0f, 255);
    render_blit_scaled(A_BASE,   0, 1080.0f - 236.0f, 6.0f, 255);
    render_blit_scaled(A_BIRD_MID, 300.0f, 540.0f, 6.0f, 255);
    render_text_center(280, "GET READY", 0xFFFFC030u, 10);
    render_text_center(400, "X TO JUMP", 0xFFFFFFFFu, 4);
    render_text_center(1080 - 340, "O TO GO BACK", 0xFF808080u, 3);
}

static void draw_gameover(void) {
    render_text_center(400, "GAME OVER", 0xFFFF3030u, 10);
    char s[64];
    snprintf(s, sizeof(s), "SCORE: %d", game.score);
    render_text_center(560, s, 0xFFFFFFFFu, 6);
    snprintf(s, sizeof(s), "BEST: %d", game.high_score);
    render_text_center(650, s, 0xFFFFC030u, 6);
    render_text_center(900, "X RESTART   O BACK", 0xFFC0C0C0u, 4);
}

static void draw_playing(void) {
    /* Background: tiled */
    int bg = game.is_night ? A_BG_NIGHT : A_BG_DAY;
    render_blit_scaled_bg(bg, game.bg_scroll - 1728.0f, 6.0f);
    render_blit_scaled_bg(bg, game.bg_scroll, 6.0f);

    /* pipes (scale 6) */
    for (int i = 0; i < game.active_count; i++) {
        struct pipe_pair *p = game.active[i];
        render_blit_scaled(A_PIPE_TOP, p->x, p->y_top - 320.0f * 6.0f, 6.0f, 255);
        render_blit_scaled(A_PIPE_BOT, p->x, p->y_bot,               6.0f, 255);
    }

    /* base strip */
    int bw = 336 * 6;
    int bx = ((int)game.base_scroll) % bw;
    if (bx > 0) bx -= bw;
    for (int x = bx; x < SCR_W; x += bw)
        render_blit_scaled(A_BASE, (float)x, 1080.0f - 236.0f, 6.0f, 255);

    /* bird sprite selection */
    int bird_asset = A_BIRD_MID;
    if (game.bird_vy < -120.0f)      bird_asset = A_BIRD_UP;
    else if (game.bird_vy > 120.0f)  bird_asset = A_BIRD_DOWN;
    render_blit_scaled(bird_asset, 300.0f, game.bird_y, 6.0f, 255);

    /* HUD */
    char s[32];
    snprintf(s, sizeof(s), "SCORE: %d", game.score);
    render_text(40, 40, s, 0xFFFFFFFFu, 5);
    snprintf(s, sizeof(s), "BEST: %d", game.high_score);
    render_text(40, 100, s, 0xFFFFC030u, 4);
}

/* --------------------------------------------------------------------- */

static void menu_update(u32 pressed) {
    if (pressed & DS_UP)
        game.menu_cursor = (game.menu_cursor + MENU_COUNT - 1) % MENU_COUNT;
    if (pressed & DS_DOWN)
        game.menu_cursor = (game.menu_cursor + 1) % MENU_COUNT;

    /* LEFT/RIGHT adjust for rows that support it */
    if (pressed & (DS_LEFT | DS_RIGHT)) {
        int dir = (pressed & DS_RIGHT) ? 1 : -1;
        if (game.menu_cursor == 1) {
            int d = (game.diff + DIFF_COUNT + dir) % DIFF_COUNT;
            game_set_diff(&game, (enum diff)d);
            save_write(&game);
        } else if (game.menu_cursor == 2) {
            game.is_night ^= 1;
            save_write(&game);
        }
    }

    if (pressed & DS_CROSS) {
        switch (game.menu_cursor) {
        case 0: game_start(&game); break;
        case 1: {
            int d = (game.diff + 1) % DIFF_COUNT;
            game_set_diff(&game, (enum diff)d);
            save_write(&game);
            break;
        }
        case 2:
            game.is_night ^= 1;
            save_write(&game);
            break;
        case 3:
            game.high_score = 0;
            game.last_score = 0;
            save_write(&game);
            break;
        case 4:
            /* informational only */
            break;
        case 5:
            save_write(&game);
            for (;;) sleep_ms(100);
        }
    }
}

static void game_update_and_draw(u32 pressed, float dt) {
    if (game.state == GS_READY) {
        game_update(&game, dt);
        if (pressed & DS_CROSS) game_jump(&game);
        if (pressed & DS_CIRCLE) { game.state = GS_MENU; return; }
        draw_playing();
        render_text_center(280, "GET READY", 0xFFFFC030u, 10);
        return;
    }

    if (game.state == GS_PLAYING) {
        game_update(&game, dt);
        if (pressed & DS_CROSS) game_jump(&game);
        if (pressed & DS_OPTIONS) game.state = GS_PAUSED;
        draw_playing();
        return;
    }

    if (game.state == GS_PAUSED) {
        draw_playing();
        render_fill_rect(0, 0, SCR_W, SCR_H, 0x80000000u);
        render_text_center(480, "PAUSED", 0xFFFFFFFFu, 10);
        render_text_center(620, "X RESUME   O BACK", 0xFFC0C0C0u, 4);
        if (pressed & DS_CROSS) game.state = GS_PLAYING;
        if (pressed & DS_CIRCLE) { game.state = GS_MENU; }
        return;
    }

    if (game.state == GS_GAMEOVER) {
        draw_playing();
        render_fill_rect(0, 0, SCR_W, SCR_H, 0xC0000000u);
        draw_gameover();
        if (pressed & DS_CROSS) { save_write(&game); game_start(&game); }
        if (pressed & DS_CIRCLE) { save_write(&game); game.state = GS_MENU; }
        return;
    }
}

/* --------------------------------------------------------------------- */

static void audio_pump(void) {
    audio_mix_tick();
}

static void *audio_thread_entry(void *arg) {
    (void)arg;
    for (;;) audio_pump();
    return 0;
}

/* --------------------------------------------------------------------- */

__attribute__((section(".text._start")))
void _start(u64 eboot, void *dlsym, struct ext_args_lua *ext) {
    G = (void*)(eboot + GADGET_OFFSET);
    D = dlsym;

    /* Video hijack first: cancels GS thread and closes emulator handle */
    if (video_init(eboot) != 0) {
        for (;;) sleep_ms(1000);
    }

    ps_libc_init(G, D,
        SYM(G, D, LIBKERNEL_HANDLE, "mmap"),
        SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen"),
        SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead"),
        SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite"),
        SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose"),
        SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLseek"),
        SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir"),
        SYM(G, D, LIBKERNEL_HANDLE, "sendto"),
        ext->log_fd, ext->log_sa);

    pad_init_from();
    audio_init_from();
    get_proc_time = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetProcessTime");
    if (get_proc_time) start_us = NC(G, get_proc_time, 0,0,0,0,0,0);

    save_init();
    game_init(&game);
    save_load(&game);

    printf("FlappyBird: video_h=%d pad=%d save=%d\n",
           video_h, pad_h, save_available());

    /* spawn audio thread if pthread_create is available */
    void *pc = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCreate");
    if (pc) {
        u64 tid = 0;
        NC(G, pc, (u64)&tid, 0,
           (u64)(void*)audio_thread_entry, 0, (u64)"flap_aud", 0);
    }

    /* Ensure first read has no false press */
    pad_prev = read_pad();

    u32 last_ms = now_ms();

    for (;;) {
        u32 cur_ms = now_ms();
        u32 dt_ms = cur_ms - last_ms;
        if (dt_ms > 50) dt_ms = 50;
        last_ms = cur_ms;
        float dt = (float)dt_ms / 1000.0f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        u32 pressed = pad_pressed();

        if (game.state == GS_MENU) {
            menu_update(pressed);
            draw_menu();
        } else {
            game_update_and_draw(pressed, dt);
        }

        present();
        if (!pc) audio_pump();   /* fallback if thread wasn't spawned */
    }
}
