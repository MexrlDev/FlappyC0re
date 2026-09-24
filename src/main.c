/* SPDX-License-Identifier: MIT */
#include "core.h"
#include "ps_libc.h"
#include "assets.h"
#include "render.h"
#include "game.h"
#include "audio.h"
#include "save.h"

struct ext_args_lua {
    u64 status;
    u64 step;
    u32 frame;
    u32 pad;
    s32 log_fd;
    s32 pad2;
    u8  log_sa[16];
    u64 tcp_srv;
    u64 wad_port;
    u64 user_id;
};

PERSIST static void *G, *D;
PERSIST static s32 g_user_id = 1;
PERSIST static volatile int g_exit_now = 0;

/* ---------------- early diagnostic ---------------- */

static void early_send(u64 eboot, void *dlsym, s32 log_fd,
                       const u8 *log_sa, const char *msg, int msg_len)
{
    if (log_fd < 0 || !log_sa) return;

    char sname[8];
    sname[0]='s'; sname[1]='e'; sname[2]='n'; sname[3]='d';
    sname[4]='t'; sname[5]='o'; sname[6]=0; sname[7]=0;

    void *sendto_fn = 0;
    void *g = (void*)(eboot + GADGET_OFFSET);
    native_call(g, dlsym, (u64)LIBKERNEL_HANDLE, (u64)sname,
                (u64)&sendto_fn, 0, 0, 0);

    if (sendto_fn)
        native_call(g, sendto_fn, (u64)log_fd, (u64)msg,
                    (u64)msg_len, 0, (u64)log_sa, 16);
}

static void early_send_hexnum(u64 eboot, void *dlsym, s32 log_fd,
                              const u8 *log_sa, const char *prefix,
                              u64 value)
{
    char buf[64];
    int p = 0;
    while (*prefix && p < 40) buf[p++] = *prefix++;

    char tmp[20]; int t = 0;
    if (value == 0) tmp[t++] = '0';
    while (value > 0) {
        int digit = (int)(value & 0xF);
        tmp[t++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        value >>= 4;
    }
    while (t > 0) buf[p++] = tmp[--t];
    buf[p++] = '\n';
    early_send(eboot, dlsym, log_fd, log_sa, buf, p);
}

/* ---------------- ELF relocations ---------------- */

typedef struct {
    u64 r_offset;
    u64 r_info;
    s64 r_addend;
} Elf64_Rela;

#define R_X86_64_64       1
#define R_X86_64_RELATIVE 8

static int apply_relocations(void) {
    u64 rs, re, base;
    __asm__ volatile("lea __rela_start(%%rip), %0" : "=r"(rs));
    __asm__ volatile("lea __rela_end(%%rip),   %0" : "=r"(re));
    __asm__ volatile("lea _start(%%rip),       %0" : "=r"(base));

    if (re <= rs) return 0;

    int count = 0;
    for (Elf64_Rela *r = (Elf64_Rela*)rs; (u64)r < re; r++) {
        u32 type = (u32)(r->r_info & 0xFFFFFFFFu);
        u64 *slot = (u64 *)(base + r->r_offset);

        if (type == R_X86_64_RELATIVE) {
            *slot = base + (u64)r->r_addend;
            count++;
        } else if (type == R_X86_64_64) {
            *slot += base;
            count++;
        }
    }
    return count;
}

/* ---------------- video ---------------- */

PERSIST static s32 video_h = -1;
PERSIST static void *vid_flip, *vid_open, *vid_close, *vid_reg, *vid_rate, *vid_evt;
PERSIST static u64 eq;
PERSIST static void *wait_eq, *create_eq, *delete_eq;
PERSIST static u8 *fbs_mem;
PERSIST static u64 start_us;
PERSIST static void *get_proc_time;
PERSIST static u64 total_frames;

/* ---------------- pad ---------------- */

PERSIST static s32 pad_h = -1;
PERSIST static void *pad_read_fn;
PERSIST static void *pad_vib_fn;
PERSIST static void *pad_lb_fn;
PERSIST static u8 pad_buf[128];
PERSIST static u32 pad_prev;
PERSIST static u8 vib_data[8];

/* ---------------- game ---------------- */

PERSIST static struct game game;

PERSIST static u32 haptic_until_ms;
PERSIST static u8  haptic_large, haptic_small;

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

/* ---------------- lightbar (stack-local struct) ---------------- */

static void lightbar(u8 r, u8 g, u8 b) {
    if (pad_h < 0 || !pad_lb_fn) return;
    struct { u8 r, g, b, x; } col;
    col.r = r;
    col.g = g;
    col.b = b;
    col.x = 0;
    NC(G, pad_lb_fn, (u64)pad_h, (u64)&col, 0,0,0,0);
}

static const u32 LB_MENU    = 0xFFDC00u;   /* yellow */
static const u32 LB_PLAYING = 0xFFDC00u;   /* yellow */
static const u32 LB_DEAD    = 0xFF0000u;   /* red */
static const u32 LB_DEFAULT = 0x0000C8u;   /* Sony soft blue */

static void lightbar_apply(u32 rgb) {
    lightbar((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/* ---------------- haptics ---------------- */

static void haptic_raw(u8 large, u8 small) {
    haptic_large = large;
    haptic_small = small;
    if (!game.vibration_on) {
        if (pad_h >= 0 && pad_vib_fn) {
            vib_data[0] = 0; vib_data[1] = 0;
            for (int i = 2; i < 8; i++) vib_data[i] = 0;
            NC(G, pad_vib_fn, (u64)pad_h, (u64)vib_data, 0,0,0,0);
        }
        return;
    }
    if (pad_h < 0 || !pad_vib_fn) return;
    vib_data[0] = large;
    vib_data[1] = small;
    for (int i = 2; i < 8; i++) vib_data[i] = 0;
    NC(G, pad_vib_fn, (u64)pad_h, (u64)vib_data, 0,0,0,0);
}

static void haptic_low_pulse(void) {
    haptic_raw(80, 80);
    haptic_until_ms = now_ms() + 40;
}

static void haptic_death(void) {
    haptic_raw(255, 255);
    haptic_until_ms = now_ms() + 1000;
}

static void haptic_restart(void) {
    haptic_raw(128, 128);
    haptic_until_ms = now_ms() + 200;
}

static void haptic_tick(void) {
    if (haptic_until_ms && now_ms() >= haptic_until_ms) {
        haptic_until_ms = 0;
        haptic_raw(0, 0);
    }
}

static void haptic_apply_toggle(void) {
    if (!game.vibration_on) {
        haptic_until_ms = 0;
        if (pad_h >= 0 && pad_vib_fn) {
            vib_data[0] = 0; vib_data[1] = 0;
            for (int i = 2; i < 8; i++) vib_data[i] = 0;
            NC(G, pad_vib_fn, (u64)pad_h, (u64)vib_data, 0,0,0,0);
        }
    }
}

/* ---------------- present / video init ---------------- */

static void present(void) {
    NC(G, vid_flip, (u64)video_h, (u64)(total_frames & 1), 1,
       (u64)total_frames, 0, 0);
    if (eq && wait_eq) {
        u8 evt[64]; s32 n = 0;
        NC(G, wait_eq, eq, (u64)evt, 1, (u64)&n, 0, 0);
    } else {
        sleep_ms(16);
    }
    render_swap();
    total_frames++;
}

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

/* ---------------- user id + audio + pad init ---------------- */

static void query_real_user_id(void) {
    void *load_mod = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    if (!load_mod) { printf("query_uid: no load_mod\n"); return; }

    s32 usr_mod = (s32)NC(G, load_mod, (u64)"libSceUserService.sprx",
                          0,0,0,0,0);
    if (usr_mod < 0) { printf("query_uid: load usr_mod failed %d\n", usr_mod); return; }

    void *get_init = SYM(G, D, usr_mod, "sceUserServiceGetInitialUser");
    void *get_fg   = SYM(G, D, usr_mod, "sceUserServiceGetForegroundUser");

    printf("query_uid: init=%p fg=%p\n", (void*)get_init, (void*)get_fg);

    s32 uid = 0;
    if (get_init) {
        s32 r = (s32)NC(G, get_init, (u64)&uid, 0,0,0,0,0);
        printf("query_uid: GetInitialUser ret=%d uid=%d\n", r, uid);
        if (r == 0 && uid > 0) { g_user_id = uid; return; }
    }
    if (get_fg) {
        uid = 0;
        s32 r = (s32)NC(G, get_fg, (u64)&uid, 0,0,0,0,0);
        printf("query_uid: GetForegroundUser ret=%d uid=%d\n", r, uid);
        if (r == 0 && uid > 0) { g_user_id = uid; return; }
    }
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
    if (pmod < 0) { printf("pad_init: load libScePad failed %d\n", pmod); return; }

    void *p_init = SYM(G, D, pmod, "scePadInit");
    void *p_geth = SYM(G, D, pmod, "scePadGetHandle");
    pad_read_fn  = SYM(G, D, pmod, "scePadRead");
    pad_vib_fn   = SYM(G, D, pmod, "scePadSetVibration");
    pad_lb_fn    = SYM(G, D, pmod, "scePadSetLightBar");

    printf("pad syms: init=%p geth=%p read=%p vib=%p lb=%p\n",
           (void*)p_init, (void*)p_geth, (void*)pad_read_fn,
           (void*)pad_vib_fn, (void*)pad_lb_fn);

    if (p_init) NC(G, p_init, 0,0,0,0,0,0);

    if (p_geth) {
        s32 candidates[6];
        int n = 0;
        candidates[n++] = g_user_id;
        candidates[n++] = 1;
        candidates[n++] = 0;
        candidates[n++] = 0xFF;
        candidates[n++] = 0xFE;
        candidates[n++] = 0x10000000;

        for (int i = 0; i < n && pad_h < 0; i++) {
            s32 uid = candidates[i];
            pad_h = (s32)NC(G, p_geth, (u64)uid, 0, 0, 0, 0, 0);
            printf("pad try uid=%d -> handle=%d\n", uid, pad_h);
        }
    }

    printf("pad handle = %d\n", pad_h);

    for (int i = 0; i < 8; i++) vib_data[i] = 0;
    if (pad_h >= 0 && pad_vib_fn)
        NC(G, pad_vib_fn, (u64)pad_h, (u64)vib_data, 0,0,0,0);

    /* Yellow lightbar as soon as we take over.  Try a few times in case
       the pad handle needs a moment to fully attach. */
    for (int i = 0; i < 5; i++) {
        lightbar_apply(LB_MENU);
        sleep_ms(30);
    }
    printf("lightbar set to yellow (pad_h=%d lb=%p)\n",
           pad_h, (void*)pad_lb_fn);
}

/* ---------------- menu UI ---------------- */

static const char *menu_items[] = {
    "START GAME",
    "DIFFICULTY",
    "BACKGROUND",
    "VIBRATION",
    "RESET SCORE",
    "SAVE STATUS",
    "CREDITS",
    "EXIT",
};
#define MENU_COUNT 8

static void draw_credits(void) {
    render_clear(0xFF0A0A0A);
    render_text_center(60,  "CREDITS", 0xFFFFC030u, 10);

    render_text_center(160, "PROGRAMMER", 0xFF808080u, 4);
    render_text_center(210, "MexrlDev",    0xFFFFFFFFu, 6);

    render_text_center(320, "SPECIAL THANKS", 0xFF808080u, 4);
    render_text_center(370, "Egycnq  -  EmuC0re / DooMC0re", 0xFFD0D0D0u, 4);
    render_text_center(410, "Gezine  -  LuaC0re",            0xFFD0D0D0u, 4);

    render_text_center(510, "ASSETS", 0xFF808080u, 4);
    render_text_center(560, "Samuel Custodio (MIT)", 0xFFD0D0D0u, 4);

    render_text_center(880, "X or O to return", 0xFFC0C0C0u, 4);
}

static void draw_menu(void) {
    if (game.show_credits) { draw_credits(); return; }

    render_clear(0xFF0A0A0A);

    render_text_center(80,  "FLAPPY BIRD", 0xFF000000u, 10);
    render_text_center(74,  "FLAPPY BIRD", 0xFFFFC030u, 10);
    render_text_center(240, "PS4/PS5 PORT", 0xFF808080u, 4);

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
            snprintf(line, sizeof(line), "VIBRATION: %s", game.vibration_on ? "ON" : "OFF");
            text = line;
        } else if (i == 4) {
            snprintf(line, sizeof(line), "RESET SCORE (%d)", game.high_score);
            text = line;
        } else if (i == 5) {
            snprintf(line, sizeof(line), "SAVE: %s",
                     save_available() ? "OK" : "NO SAVEDATA");
            text = line;
        }
        if (i == game.menu_cursor)
            render_text(160, base_y + i * 60 - 6, ">", col, 4);
        render_text(240, base_y + i * 60, text, col, 4);
    }

    render_text(40, 940, "X: SELECT   O: BACK", 0xFF808080u, 3);
    render_text(SCR_W - 40 - render_text_width("By MexrlDev", 3),
                940, "By MexrlDev", 0xFF606060u, 3);

    char hi[64];
    snprintf(hi, sizeof(hi), "HIGH: %d   LIFETIME: %u",
             game.high_score, game.lifetime_pipes);
    render_text(40, 990, hi, 0xFF404040u, 3);
}

static void draw_playing(void) {
    int bg = game.is_night ? A_BG_NIGHT : A_BG_DAY;
    render_blit_scaled_bg(bg, game.bg_scroll - 1728.0f, 6.0f);
    render_blit_scaled_bg(bg, game.bg_scroll, 6.0f);

    for (int i = 0; i < game.active_count; i++) {
        struct pipe_pair *p = game.active[i];
        render_blit_scaled(A_PIPE_TOP, p->x, p->y_top - 320.0f * 6.0f, 6.0f, 255);
        render_blit_scaled(A_PIPE_BOT, p->x, p->y_bot,               6.0f, 255);
    }

    int bw = 336 * 6;
    int bx = ((int)game.base_scroll) % bw;
    if (bx > 0) bx -= bw;
    for (int x = bx; x < SCR_W; x += bw)
        render_blit_scaled(A_BASE, (float)x, 1080.0f - 236.0f, 6.0f, 255);

    int bird_asset = A_BIRD_MID;
    if (game.bird_vy < -120.0f)      bird_asset = A_BIRD_UP;
    else if (game.bird_vy > 120.0f)  bird_asset = A_BIRD_DOWN;
    render_blit_scaled(bird_asset, 300.0f, game.bird_y, 6.0f, 255);

    char s[32];
    snprintf(s, sizeof(s), "SCORE: %d", game.score);
    render_text(40, 40, s, 0xFFFFFFFFu, 5);
    snprintf(s, sizeof(s), "BEST: %d", game.high_score);
    render_text(40, 100, s, 0xFFFFC030u, 4);
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

static void draw_ready(void) {
    draw_playing();
    render_text_center(280, "GET READY",   0xFFFFC030u, 10);
    render_text_center(400, "X TO JUMP",   0xFFFFFFFFu, 4);
    render_text_center(1080 - 340, "O TO GO BACK", 0xFF808080u, 3);
}

/* ---------------- menu logic ---------------- */

static void menu_update(u32 pressed) {
    if (game.show_credits) {
        if (pressed & (DS_CROSS | DS_CIRCLE))
            game.show_credits = 0;
        return;
    }

    if (pressed & DS_UP)
        game.menu_cursor = (game.menu_cursor + MENU_COUNT - 1) % MENU_COUNT;
    if (pressed & DS_DOWN)
        game.menu_cursor = (game.menu_cursor + 1) % MENU_COUNT;

    if (pressed & (DS_LEFT | DS_RIGHT)) {
        int dir = (pressed & DS_RIGHT) ? 1 : -1;
        if (game.menu_cursor == 1) {
            int d = (game.diff + DIFF_COUNT + dir) % DIFF_COUNT;
            game_set_diff(&game, (enum diff)d);
            save_write(&game);
        } else if (game.menu_cursor == 2) {
            game.is_night ^= 1;
            save_write(&game);
        } else if (game.menu_cursor == 3) {
            game.vibration_on ^= 1;
            haptic_apply_toggle();
            save_write(&game);
        }
    }

    if (pressed & DS_CROSS) {
        switch (game.menu_cursor) {
        case 0: game_start(&game); break;
        case 1:
            game_set_diff(&game, (enum diff)((game.diff + 1) % DIFF_COUNT));
            save_write(&game);
            break;
        case 2:
            game.is_night ^= 1;
            save_write(&game);
            break;
        case 3:
            game.vibration_on ^= 1;
            haptic_apply_toggle();
            save_write(&game);
            break;
        case 4:
            game.high_score = 0;
            game.last_score = 0;
            save_write(&game);
            break;
        case 5:
            break;
        case 6:
            game.show_credits = 1;
            break;
        case 7:
            /* Signal the main loop to tear down and return to LuaC0re. */
            save_write(&game);
            g_exit_now = 1;
            break;
        }
    }
}

/* ---------------- game loop body ---------------- */

PERSIST static enum gstate last_gstate = 0xFF;

static void game_update_and_draw(u32 pressed, float dt) {
    if (game.state != last_gstate) {
        if (game.state == GS_GAMEOVER) {
            haptic_death();
            lightbar_apply(LB_DEAD);
        } else if (game.state == GS_MENU) {
            lightbar_apply(LB_MENU);
        } else {
            lightbar_apply(LB_PLAYING);
        }
        last_gstate = game.state;
    }

    if (game.state == GS_READY) {
        game_update(&game, dt);
        if (pressed & DS_CROSS) { game_jump(&game); haptic_low_pulse(); }
        if (pressed & DS_CIRCLE) game.state = GS_MENU;
        draw_ready();
        return;
    }

    if (game.state == GS_PLAYING) {
        game_update(&game, dt);
        if (pressed & DS_CROSS)   { game_jump(&game); haptic_low_pulse(); }
        if (pressed & DS_OPTIONS) game.state = GS_PAUSED;
        draw_playing();
        return;
    }

    if (game.state == GS_PAUSED) {
        draw_playing();
        render_fill_rect(0, 0, SCR_W, SCR_H, 0x80000000u);
        render_text_center(480, "PAUSED", 0xFFFFFFFFu, 10);
        render_text_center(620, "X RESUME   O BACK", 0xFFC0C0C0u, 4);
        if (pressed & DS_CROSS)  game.state = GS_PLAYING;
        if (pressed & DS_CIRCLE) game.state = GS_MENU;
        return;
    }

    if (game.state == GS_GAMEOVER) {
        draw_playing();
        render_fill_rect(0, 0, SCR_W, SCR_H, 0xC0000000u);
        draw_gameover();
        if (pressed & DS_CROSS) {
            save_write(&game);
            haptic_restart();
            game_start(&game);
        }
        if (pressed & DS_CIRCLE) {
            save_write(&game);
            game.state = GS_MENU;
        }
        return;
    }
}

/* ---------------- audio pump ---------------- */

static void audio_pump(void) { audio_mix_tick(); }

static void *audio_thread_entry(void *arg) {
    (void)arg;
    for (;;) audio_pump();
    return 0;
}

/* ---------------- cleanup ---------------- */

static void cleanup_and_return(struct ext_args_lua *ext) {
    /* 1. Kill vibration */
    if (pad_h >= 0 && pad_vib_fn) {
        vib_data[0] = 0; vib_data[1] = 0;
        for (int i = 2; i < 8; i++) vib_data[i] = 0;
        NC(G, pad_vib_fn, (u64)pad_h, (u64)vib_data, 0,0,0,0);
    }

    /* 2. Restore Sony soft-blue lightbar */
    lightbar_apply(LB_DEFAULT);
    sleep_ms(80);

    /* 3. Stop audio */
    audio_shutdown();

    /* 4. Blank the screen so the next payload starts on a clean slate */
    if (fbs_mem) {
        u32 *fb0 = (u32*)fbs_mem;
        u32 *fb1 = (u32*)(fbs_mem + FB_ALIGNED);
        for (int i = 0; i < SCR_W * SCR_H; i++) { fb0[i] = 0xFF000000; fb1[i] = 0xFF000000; }
        if (vid_flip && video_h >= 0)
            NC(G, vid_flip, (u64)video_h, 0, 1, 0, 0, 0);
        sleep_ms(50);
    }

    /* 5. Close video */
    if (vid_close && video_h >= 0)
        NC(G, vid_close, (u64)video_h, 0,0,0,0,0);

    /* 6. Delete event queue */
    if (delete_eq && eq)
        NC(G, delete_eq, eq, 0,0,0,0,0);

    /* 7. Tell the Lua side we finished cleanly */
    ext->status = 0;
    ext->step   = 99;
    ext->frame  = (u32)total_frames;
}

/* ---------------- entry point ---------------- */

__attribute__((section(".text._start")))
void _start(u64 eboot, void *dlsym, struct ext_args_lua *ext) {
    int nreloc = apply_relocations();

    G = (void*)(eboot + GADGET_OFFSET);
    D = dlsym;

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "ENTRY\n", 6);
    early_send_hexnum(eboot, dlsym, ext->log_fd, ext->log_sa,
                      "RELOC ", (u64)nreloc);

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "VIDEO\n", 6);
    if (video_init(eboot) != 0) {
        early_send(eboot, dlsym, ext->log_fd, ext->log_sa,
                   "VIDEO FAIL\n", 11);
        for (;;) sleep_ms(1000);
    }

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "LIBC\n", 5);
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

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "UID\n", 4);
    query_real_user_id();
    early_send_hexnum(eboot, dlsym, ext->log_fd, ext->log_sa,
                      "UIDFINAL ", (u64)g_user_id);

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "PAD\n", 4);
    pad_init_from();

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "AUDIO\n", 6);
    audio_init_from();

    get_proc_time = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetProcessTime");
    if (get_proc_time) start_us = NC(G, get_proc_time, 0,0,0,0,0,0);

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "SAVE\n", 5);
    save_init();
    game_init(&game);
    game.vibration_on = 1;
    save_load(&game);

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "READY\n", 6);

    printf("FlappyBird: relocs=%d video_h=%d pad_h=%d vib=%d lb=%d save=%d uid=%d\n",
           nreloc, video_h, pad_h,
           pad_vib_fn ? 1 : 0, pad_lb_fn ? 1 : 0,
           save_available(), (int)g_user_id);

    void *pc = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCreate");
    if (pc) {
        u64 tid = 0;
        NC(G, pc, (u64)&tid, 0,
           (u64)(void*)audio_thread_entry, 0, (u64)"flap_aud", 0);
    }

    pad_prev = read_pad();

    u32 last_ms = now_ms();

    while (!g_exit_now) {
        u32 cur_ms = now_ms();
        u32 dt_ms = cur_ms - last_ms;
        if (dt_ms > 50) dt_ms = 50;
        last_ms = cur_ms;
        float dt = (float)dt_ms / 1000.0f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        haptic_tick();

        /* Light pad logging for the first 5 s only. */
        if (total_frames < 300 && (total_frames % 60) == 0) {
            u32 raw = read_pad();
            printf("PAD f=%u raw=%08x state=%d\n",
                   (unsigned)total_frames, raw, (int)game.state);
        }

        u32 pressed = pad_pressed();

        if (game.state == GS_MENU) {
            menu_update(pressed);
            draw_menu();
        } else {
            game_update_and_draw(pressed, dt);
        }

        present();
        if (!pc) audio_pump();
    }

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "EXIT\n", 5);
    cleanup_and_return(ext);

    /* Return to LuaC0re.  The Lua script ends after func_wrap, so control
       passes back to the loader. */
    return;
}
