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

PERSIST static volatile int g_audio_running = 0;
PERSIST static volatile int g_audio_pause   = 0;
PERSIST static u64 g_audio_tid = 0;
PERSIST static s32 g_aud_handle = -1;
PERSIST static void *g_aud_close_fn = 0;
PERSIST static void *g_aud_open_fn  = 0;
PERSIST static void *g_aud_out_fn   = 0;
PERSIST static void *g_aud_mod      = 0;

PERSIST static u8 g_aud_actual_port = 2;

/* Swap state machine, fully non-blocking.  Never times out on failure -
   keeps trying both ports until one opens, so audio never stays dead. */
#define SWAP_IDLE    0
#define SWAP_CLOSE   1
#define SWAP_TRY     2

PERSIST static volatile int g_swap_active = 0;
PERSIST static int g_swap_phase = SWAP_IDLE;
PERSIST static u8  g_swap_target = 2;
PERSIST static u8  g_swap_old    = 2;
PERSIST static u32 g_swap_step_at = 0;
PERSIST static u32 g_swap_cooldown_until = 0;
PERSIST static u32 g_swap_last_log = 0;

PERSIST static s32 g_pad_mod = -1;
PERSIST static u32 g_pad_fails = 0;

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
PERSIST static void *pad_vib_fn;
PERSIST static void *pad_lb_fn;
PERSIST static u8 pad_buf[128];
PERSIST static u32 pad_prev;
PERSIST static u8 vib_data[8];

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
    if (pad_h < 0 || !pad_read_fn) { g_pad_fails++; return 0; }
    for (int i = 0; i < 128; i++) pad_buf[i] = 0;
    s32 r = (s32)NC(G, pad_read_fn, (u64)pad_h, (u64)pad_buf, 1, 0, 0, 0);
    if (r <= 0) { g_pad_fails++; return 0; }
    u32 raw = *(u32*)pad_buf;
    if (raw & 0x80000000u) { g_pad_fails++; return 0; }
    g_pad_fails = 0;
    return raw & 0x001FFFFFu;
}

static void lightbar(u8 r, u8 g, u8 b) {
    if (pad_h < 0 || !pad_lb_fn) return;
    struct { u8 r, g, b, x; } col;
    col.r = r; col.g = g; col.b = b; col.x = 0;
    NC(G, pad_lb_fn, (u64)pad_h, (u64)&col, 0,0,0,0);
}

static const u32 LB_MENU    = 0xFF8000u;
static const u32 LB_PLAYING = 0xFFFF00u;
static const u32 LB_DEAD    = 0xFF0000u;
static const u32 LB_DEFAULT = 0x0000C8u;

static void lightbar_apply(u32 rgb) {
    lightbar((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static u32 rainbow_color(u32 tick) {
    u32 t = tick & 0x5FF;
    u32 r, g, b;
    if      (t < 256)  { r = 255;       g = t;          b = 0; }
    else if (t < 512)  { r = 511 - t;   g = 255;        b = 0; }
    else if (t < 768)  { r = 0;         g = 255;        b = t - 512; }
    else if (t < 1024) { r = 0;         g = 1023 - t;   b = 255; }
    else if (t < 1280) { r = t - 1024;  g = 0;          b = 255; }
    else               { r = 255;       g = 0;          b = 1535 - t; }
    return (r << 16) | (g << 8) | b;
}

static u32 g_last_lb = 0xFFFFFFFFu;
static void update_lightbar(void) {
    u32 desired;
    if (game.state == GS_PAUSED) {
        desired = rainbow_color(total_frames * 16u);
    } else if (game.state == GS_GAMEOVER) {
        desired = LB_DEAD;
    } else if (game.state == GS_PLAYING || game.state == GS_READY) {
        desired = LB_PLAYING;
    } else {
        desired = LB_MENU;
    }
    if (desired == g_last_lb) return;
    g_last_lb = desired;
    lightbar_apply(desired);
}

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

static void haptic_low_pulse(void) { haptic_raw(80, 80);   haptic_until_ms = now_ms() + 40;   }
static void haptic_death(void)     { haptic_raw(255, 255); haptic_until_ms = now_ms() + 1000; }
static void haptic_restart(void)   { haptic_raw(128, 128); haptic_until_ms = now_ms() + 200;  }

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

static void apply_screen_viewport(void) {
    switch (game.screen_mode) {
    case SCREEN_FULL:
        render_set_viewport(1.0f, 0, 0);
        break;
    case SCREEN_16_9:
        render_set_viewport(0.833333f, 160, 90);
        break;
    case SCREEN_4_3:
        render_set_viewport(0.75f, 240, 135);
        break;
    case SCREEN_MODE_COUNT:
    default:
        render_set_viewport(1.0f, 0, 0);
        break;
    }
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
    render_swap();
    total_frames++;
}

static int video_init(u64 eboot) {
    void *cancel = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    if (cancel) {
        u64 gs   = *(u64*)(eboot + EBOOT_GS_THREAD);
        u64 spu2 = *(u64*)(eboot + EBOOT_IOP_SPU2);
        printf("cancel: gs=0x%llx spu2=0x%llx\n",
               (unsigned long long)gs, (unsigned long long)spu2);
        if (gs)   NC(G, cancel, gs,   0,0,0,0,0);
        if (spu2) NC(G, cancel, spu2, 0,0,0,0,0);
    }
    sleep_ms(500);

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
    render_clear_full(0xFF000000);
    render_swap();
    render_clear_full(0xFF000000);
    render_swap();
    return 0;
}

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

static s32 try_open_audio_port(s32 user, s32 type) {
    if (!g_aud_open_fn) return -1;
    return (s32)NC(G, g_aud_open_fn,
                   (u64)(s64)user,
                   (u64)(s64)type,
                   0, 1024, SAMPLE_RATE, AUDIO_S16_STEREO);
}

#define PORT_TV       2
#define PORT_HEADSET  3

static u8 port_normalize(u8 p) {
    if (p == PORT_HEADSET) return PORT_HEADSET;
    return PORT_TV;
}

static int audio_sweep_stale_handles(void) {
    if (!g_aud_close_fn) return 0;
    int released = 0;
    static const u64 prefixes[] = {
        0x20000000ULL,
        0x20010000ULL,
        0x20020000ULL,
        0x20030000ULL,
    };
    for (int p = 0; p < 4; p++) {
        for (u64 i = 1; i <= 0x40; i++) {
            u64 h = prefixes[p] | i;
            s32 r = (s32)NC(G, g_aud_close_fn, h, 0,0,0,0,0);
            if (r == 0) released++;
        }
    }
    return released;
}

static void audio_init_from(void) {
    g_aud_mod = (void*)(s64)NC(G, SYM(G,D,LIBKERNEL_HANDLE,"sceKernelLoadStartModule"),
                              (u64)"libSceAudioOut.sprx", 0,0,0,0,0);
    if ((s32)(s64)g_aud_mod < 0) {
        printf("audio: load libSceAudioOut failed\n");
        return;
    }
    g_aud_open_fn  = SYM(G, D, (s32)(s64)g_aud_mod, "sceAudioOutOpen");
    g_aud_out_fn   = SYM(G, D, (s32)(s64)g_aud_mod, "sceAudioOutOutput");
    g_aud_close_fn = SYM(G, D, (s32)(s64)g_aud_mod, "sceAudioOutClose");

    printf("audio: open=%p out=%p close=%p\n",
           (void*)g_aud_open_fn, (void*)g_aud_out_fn, (void*)g_aud_close_fn);

    if (!g_aud_open_fn || !g_aud_out_fn) {
        printf("audio: syms missing\n");
        return;
    }

    int released = audio_sweep_stale_handles();
    printf("audio: released %d stale handles\n", released);
    if (released > 0) sleep_ms(800);

    /* Force TV (port 2) on startup unless the save says HEADSET. */
    u8 preferred = port_normalize(game.audio_port);
    u8 other     = (preferred == PORT_TV) ? PORT_HEADSET : PORT_TV;

    s32 h = -1;
    u8  opened = preferred;

    for (int i = 0; i < 20 && h < 0; i++) {
        if (g_user_id > 0) h = try_open_audio_port(g_user_id, (s32)preferred);
        if (h < 0) h = try_open_audio_port(0xFF, (s32)preferred);
        if (h < 0 && i < 19) sleep_ms(250);
    }
    printf("audio: try %s -> %d (0x%08x)\n",
           game_audio_port_name(preferred), h, (unsigned)h);

    if (h < 0) {
        for (int i = 0; i < 12 && h < 0; i++) {
            if (g_user_id > 0) h = try_open_audio_port(g_user_id, (s32)other);
            if (h < 0) h = try_open_audio_port(0xFF, (s32)other);
            if (h < 0 && i < 11) sleep_ms(250);
        }
        printf("audio: try %s -> %d (0x%08x)\n",
               game_audio_port_name(other), h, (unsigned)h);
        if (h >= 0) opened = other;
    }

    if (h < 0) {
        printf("audio: NO AUDIO AVAILABLE.\n");
        return;
    }

    printf("audio: handle=%d port=%s\n",
           h, game_audio_port_name(opened));

    g_aud_actual_port = opened;
    game.audio_port   = opened;

    g_aud_handle = h;
    audio_init(h, g_aud_out_fn, G);

    save_write(&game);
}

/* Is the audio subsystem currently busy (swapping or on cooldown)? */
static int audio_is_busy(void) {
    if (g_swap_active) return 1;
    if (g_swap_cooldown_until && now_ms() < g_swap_cooldown_until) return 1;
    return 0;
}

/* Request a swap.  Non-blocking.  Ignores requests during busy state. */
static void audio_swap_request(u8 target) {
    if (g_swap_active) {
        printf("audio: swap already in progress\n");
        return;
    }
    if (g_swap_cooldown_until && now_ms() < g_swap_cooldown_until) {
        printf("audio: cooldown %u ms\n",
               (unsigned)(g_swap_cooldown_until - now_ms()));
        return;
    }

    target = port_normalize(target);
    if (target == g_aud_actual_port && g_aud_handle >= 0) {
        game.audio_port = target;
        return;
    }

    printf("audio: swap -> %s\n", game_audio_port_name(target));

    g_audio_pause   = 1;
    g_swap_target   = target;
    g_swap_old      = g_aud_actual_port;
    g_swap_phase    = SWAP_CLOSE;
    g_swap_step_at  = now_ms();
    g_swap_last_log = 0;
    g_swap_active   = 1;

    game.audio_port = target;
    save_write(&game);
}

/* Non-blocking tick.  Runs every frame from the main loop.
   Never times out - keeps retrying both target and old port until one
   opens.  This guarantees audio is never left dead. */
static void audio_swap_tick(void) {
    if (!g_swap_active) return;

    u32 now = now_ms();

    if (g_swap_phase == SWAP_CLOSE) {
        /* Stop the audio thread submitting, then close the handle. */
        audio_shutdown();
        if (g_aud_handle >= 0) {
            s32 r = (s32)NC(G, g_aud_close_fn, (u64)g_aud_handle, 0,0,0,0,0);
            printf("audio: closed %d (%d)\n", g_aud_handle, r);
            g_aud_handle = -1;
        }
        g_swap_step_at = now;
        g_swap_phase   = SWAP_TRY;
        return;
    }

    if (g_swap_phase == SWAP_TRY) {
        /* Retry every 100 ms.  Try target first, then old port. */
        if (now - g_swap_step_at < 100) return;
        g_swap_step_at = now;

        u8 order[2];
        order[0] = g_swap_target;
        order[1] = g_swap_old;

        for (int i = 0; i < 2; i++) {
            u8 p = order[i];

            s32 h = -1;
            if (g_user_id > 0) h = try_open_audio_port(g_user_id, (s32)p);
            if (h < 0) h = try_open_audio_port(0xFF, (s32)p);

            if (h >= 0) {
                g_aud_handle      = h;
                g_aud_actual_port = p;
                game.audio_port   = p;
                audio_init(h, g_aud_out_fn, G);
                save_write(&game);
                printf("audio: now on %s (handle %d)\n",
                       game_audio_port_name(p), h);
                g_audio_pause   = 0;
                g_swap_active   = 0;
                g_swap_cooldown_until = now + 1500;
                return;
            }
        }

        /* Log every 2 seconds so we know it's still trying. */
        if (now - g_swap_last_log > 2000) {
            g_swap_last_log = now;
            printf("audio: still trying %s / %s\n",
                   game_audio_port_name(g_swap_target),
                   game_audio_port_name(g_swap_old));
        }
    }
}

static void pad_init_from(void) {
    s32 pmod = (s32)NC(G, SYM(G,D,LIBKERNEL_HANDLE,"sceKernelLoadStartModule"),
                       (u64)"libScePad.sprx", 0,0,0,0,0);
    if (pmod < 0) { printf("pad_init: load libScePad failed %d\n", pmod); return; }
    g_pad_mod = pmod;

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

    for (int i = 0; i < 5; i++) {
        lightbar_apply(LB_MENU);
        sleep_ms(30);
    }
    printf("lightbar set to orange (pad_h=%d lb=%p)\n",
           pad_h, (void*)pad_lb_fn);
}

/* Reset SETTINGS only.  Score and lifetime pipes are preserved. */
static void reset_settings_to_default(void) {
    printf("reset: settings (score preserved)\n");

    game_set_diff(&game, DIFF_NORMAL);
    game.is_night     = 0;
    game.vibration_on = 1;
    game.sfx_volume   = 100;
    game.screen_mode  = SCREEN_FULL;

    audio_set_master(game.sfx_volume);
    haptic_apply_toggle();

    if (g_aud_actual_port != PORT_TV || g_aud_handle < 0) {
        audio_swap_request(PORT_TV);
    } else {
        game.audio_port = PORT_TV;
    }

    save_write(&game);

    printf("reset: done, port=%s high=%d\n",
           game_audio_port_name(game.audio_port), game.high_score);
}

static const char *menu_items[] = {
    "START GAME",
    "DIFFICULTY",
    "BACKGROUND",
    "VIBRATION",
    "SFX",
    "AUDIO OUTPUT",
    "SCREEN",
    "RESET SCORE",
    "SAVE STATUS",
    "RESET SETTINGS",
    "CREDITS",
    "EXIT",
};
#define MENU_COUNT 12

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

    render_text_center(730, "In memory of PsVue-Mod", 0xFF87CEEBu, 5);

    render_text_center(900, "X or O to return", 0xFFC0C0C0u, 4);
}

static void draw_menu(void) {
    if (game.show_credits) { draw_credits(); return; }

    render_clear(0xFF0A0A0A);

    render_text_center(80,  "FLAPPY BIRD", 0xFF000000u, 10);
    render_text_center(74,  "FLAPPY BIRD", 0xFFFFC030u, 10);
    render_text_center(240, "PS4/PS5 PORT", 0xFF808080u, 4);

    int base_y = 300;
    int step   = 48;
    for (int i = 0; i < MENU_COUNT; i++) {
        char line[64];
        u32 col = (i == game.menu_cursor) ? 0xFFFFC030u : 0xFFD0D0D0u;
        const char *text = menu_items[i];

        /* Grey out AUDIO OUTPUT while swapping or on cooldown. */
        if (i == 5 && audio_is_busy()) {
            col = 0xFF606060u;
        }

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
            if      (game.sfx_volume == 100) snprintf(line, sizeof(line), "SFX: ON");
            else if (game.sfx_volume == 0)   snprintf(line, sizeof(line), "SFX: OFF");
            else                              snprintf(line, sizeof(line), "SFX: %u%%",
                                                         (unsigned)game.sfx_volume);
            text = line;
        } else if (i == 5) {
            snprintf(line, sizeof(line), "AUDIO OUTPUT: %s",
                     game_audio_port_name(game.audio_port));
            text = line;
        } else if (i == 6) {
            snprintf(line, sizeof(line), "SCREEN: %s", game_screen_name(game.screen_mode));
            text = line;
        } else if (i == 7) {
            snprintf(line, sizeof(line), "RESET SCORE (%d)", game.high_score);
            text = line;
        } else if (i == 8) {
            snprintf(line, sizeof(line), "SAVE: %s",
                     save_available() ? "OK" : "NO SAVEDATA");
            text = line;
        }
        if (i == game.menu_cursor)
            render_text(160, base_y + i * step - 6, ">", col, 4);
        render_text(240, base_y + i * step, text, col, 4);
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

    int bg_copies = (SCR_W + BG_W - 1) / BG_W + 1;
    for (int i = 0; i < bg_copies; i++) {
        render_blit_scaled_bg_fp(bg, game.bg_scroll + (float)(i * BG_W),
                                 GAME_SCALE_FP);
    }

    for (int i = 0; i < game.active_count; i++) {
        struct pipe_pair *p = game.active[i];
        render_blit_scaled_fp(A_PIPE_TOP, p->x, p->y_top - (float)PIPE_H,
                              GAME_SCALE_FP, 255);
        render_blit_scaled_fp(A_PIPE_BOT, p->x, p->y_bot,
                              GAME_SCALE_FP, 255);
    }

    int base_copies = (SCR_W + BASE_W - 1) / BASE_W + 1;
    for (int i = 0; i < base_copies; i++) {
        render_blit_scaled_fp(A_BASE,
                              game.base_scroll + (float)(i * BASE_W),
                              (float)(SCR_H - GROUND_H),
                              GAME_SCALE_FP, 255);
    }

    int bird_asset = A_BIRD_MID;
    if (game.bird_vy < -120.0f)      bird_asset = A_BIRD_UP;
    else if (game.bird_vy > 120.0f)  bird_asset = A_BIRD_DOWN;
    render_blit_scaled_fp(bird_asset, BIRD_X_POS, game.bird_y,
                          GAME_SCALE_FP, 255);

    char s[32];
    snprintf(s, sizeof(s), "SCORE: %d", game.score);
    render_text(40, 40, s, 0xFFFFFFFFu, 5);
    snprintf(s, sizeof(s), "BEST: %d", game.high_score);
    render_text(40, 100, s, 0xFFFFC030u, 4);
}

static void draw_gameover(void) {
    int gx = (SCR_W - GAMEOVER_W) / 2;
    render_blit_scaled_fp(A_GAMEOVER, (float)gx, (float)(SCR_H / 2 - 100),
                          384, 255);

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
        } else if (game.menu_cursor == 4) {
            int v = (int)game.sfx_volume + dir * 5;
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            game.sfx_volume = (u8)v;
            audio_set_master(game.sfx_volume);
            save_write(&game);
        } else if (game.menu_cursor == 5) {
            if (!audio_is_busy()) {
                u8 next = (game.audio_port == PORT_TV) ? PORT_HEADSET : PORT_TV;
                audio_swap_request(next);
            }
        } else if (game.menu_cursor == 6) {
            int m = (game.screen_mode + SCREEN_MODE_COUNT + dir) % SCREEN_MODE_COUNT;
            game.screen_mode = (enum screen_mode)m;
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
            game.sfx_volume = (game.sfx_volume == 0) ? 100 : 0;
            audio_set_master(game.sfx_volume);
            save_write(&game);
            break;
        case 5: {
            if (!audio_is_busy()) {
                u8 next = (game.audio_port == PORT_TV) ? PORT_HEADSET : PORT_TV;
                audio_swap_request(next);
            }
            break;
        }
        case 6:
            game.screen_mode = (enum screen_mode)
                ((game.screen_mode + 1) % SCREEN_MODE_COUNT);
            save_write(&game);
            break;
        case 7:
            /* Reset SCORE only. */
            game.high_score = 0;
            game.last_score = 0;
            save_write(&game);
            break;
        case 8:
            break;
        case 9:
            /* Reset SETTINGS only - score/lifetime preserved. */
            reset_settings_to_default();
            break;
        case 10:
            game.show_credits = 1;
            break;
        case 11:
            save_write(&game);
            g_exit_now = 1;
            break;
        }
    }
}

PERSIST static enum gstate last_gstate = 0xFF;

static void game_update_and_draw(u32 pressed, float dt) {
    if (game.state != last_gstate) {
        printf("STATE %d -> %d (f=%u)\n",
               (int)last_gstate, (int)game.state, (unsigned)total_frames);
        if (game.state == GS_GAMEOVER) haptic_death();
        last_gstate = game.state;
    }

    if (game.state == GS_PLAYING && g_pad_fails > 45) {
        game.state = GS_PAUSED;
        printf("PAD disconnected -> auto-pause\n");
        return;
    }

    if (game.state == GS_READY) {
        game_update(&game, dt);
        if (pressed & DS_CROSS) { game_jump(&game); haptic_low_pulse(); }
        if (pressed & DS_CIRCLE) {
            save_write(&game);
            game.state = GS_MENU;
        }
        draw_ready();
        return;
    }

    if (game.state == GS_PLAYING) {
        game_update(&game, dt);
        if (pressed & DS_CROSS)   { game_jump(&game); haptic_low_pulse(); }
        if (pressed & DS_OPTIONS) {
            if (game.score > game.high_score) game.high_score = game.score;
            save_write(&game);
            game.state = GS_PAUSED;
            return;
        }
        draw_playing();
        return;
    }

    if (game.state == GS_PAUSED) {
        draw_playing();
        render_fill_rect(0, 0, SCR_W, SCR_H, 0x80000000u);
        render_text_center(480, "PAUSED", 0xFFFFFFFFu, 10);
        render_text_center(620, "X RESUME   O BACK", 0xFFC0C0C0u, 4);
        if (pressed & DS_CROSS)   game.state = GS_PLAYING;
        if (pressed & DS_OPTIONS) game.state = GS_PLAYING;
        if (pressed & DS_CIRCLE) {
            save_write(&game);
            game.state = GS_MENU;
        }
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

static void audio_pump(void) { audio_mix_tick(); }

static void *audio_thread_entry(void *arg) {
    (void)arg;

    void *self_fn = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadSelf");
    void *setprio = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadSetprio");
    if (self_fn && setprio) {
        u64 self = NC(G, self_fn, 0,0,0,0,0,0);
        if (self) NC(G, setprio, self, 100, 0, 0, 0, 0);
    }

    void *usleep = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");

    while (g_audio_running) {
        if (g_audio_pause) {
            if (usleep) NC(G, usleep, 15000, 0,0,0,0,0);
            continue;
        }
        if (!audio_is_active()) {
            if (usleep) NC(G, usleep, 30000, 0,0,0,0,0);
            continue;
        }
        audio_pump();
    }
    return 0;
}

static void cleanup_and_return(struct ext_args_lua *ext) {
    g_audio_running = 0;
    sleep_ms(100);

    void *cancel = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    if (cancel && g_audio_tid) {
        NC(G, cancel, g_audio_tid, 0, 0, 0, 0, 0);
        sleep_ms(30);
    }
    g_audio_tid = 0;

    if (pad_h >= 0 && pad_vib_fn) {
        vib_data[0] = 0; vib_data[1] = 0;
        for (int i = 2; i < 8; i++) vib_data[i] = 0;
        NC(G, pad_vib_fn, (u64)pad_h, (u64)vib_data, 0,0,0,0);
    }

    lightbar_apply(LB_DEFAULT);
    sleep_ms(80);

    audio_shutdown();
    if (g_aud_close_fn && g_aud_handle >= 0) {
        NC(G, g_aud_close_fn, (u64)g_aud_handle, 0,0,0,0,0);
        g_aud_handle = -1;
    }
    audio_sweep_stale_handles();

    sleep_ms(2000);

    if (fbs_mem) {
        u32 *fb0 = (u32*)fbs_mem;
        u32 *fb1 = (u32*)(fbs_mem + FB_ALIGNED);
        for (int i = 0; i < SCR_W * SCR_H; i++) { fb0[i] = 0xFF000000; fb1[i] = 0xFF000000; }
        if (vid_flip && video_h >= 0)
            NC(G, vid_flip, (u64)video_h, 0, 1, 0, 0, 0);
        sleep_ms(50);
    }

    if (vid_close && video_h >= 0) {
        NC(G, vid_close, (u64)video_h, 0,0,0,0,0);
        video_h = -1;
    }

    if (delete_eq && eq) {
        NC(G, delete_eq, eq, 0,0,0,0,0);
        eq = 0;
    }

    ext->status = 0;
    ext->step   = 99;
    ext->frame  = (u32)total_frames;
}

__attribute__((section(".text._start")))
void _start(u64 eboot, void *dlsym, struct ext_args_lua *ext) {
    g_exit_now        = 0;
    g_audio_running   = 1;
    g_audio_pause     = 0;
    g_audio_tid       = 0;
    g_aud_handle      = -1;
    g_aud_close_fn    = 0;
    g_aud_open_fn     = 0;
    g_aud_out_fn      = 0;
    g_aud_mod         = 0;
    g_aud_actual_port = 2;
    g_swap_active     = 0;
    g_swap_phase      = SWAP_IDLE;
    g_swap_target     = 2;
    g_swap_old        = 2;
    g_swap_step_at    = 0;
    g_swap_cooldown_until = 0;
    g_swap_last_log   = 0;
    g_pad_mod         = -1;
    g_pad_fails       = 0;
    pad_prev          = 0;
    haptic_until_ms   = 0;
    total_frames      = 0;
    video_h           = -1;
    fbs_mem           = 0;
    eq                = 0;
    g_last_lb         = 0xFFFFFFFFu;
    last_gstate       = 0xFF;

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

    save_init();
    game_init(&game);
    game.vibration_on = 1;
    game.sfx_volume   = 100;
    game.audio_port   = PORT_TV;
    save_load(&game);

    game.audio_port = port_normalize(game.audio_port);

    audio_set_master(game.sfx_volume);

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "AUDIO\n", 6);
    audio_init_from();

    get_proc_time = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetProcessTime");
    if (get_proc_time) start_us = NC(G, get_proc_time, 0,0,0,0,0,0);

    early_send(eboot, dlsym, ext->log_fd, ext->log_sa, "READY\n", 6);

    printf("FlappyBird: relocs=%d video_h=%d pad_h=%d vib=%d lb=%d save=%d uid=%d mode=%s sfx=%u port=%s\n",
           nreloc, video_h, pad_h,
           pad_vib_fn ? 1 : 0, pad_lb_fn ? 1 : 0,
           save_available(), (int)g_user_id,
           game_screen_name(game.screen_mode),
           (unsigned)game.sfx_volume,
           game_audio_port_name(game.audio_port));

    printf("Layout: BG=%dx%d BASE=%dx%d PIPE=%dx%d BIRD=%dx%d GND_H=%d SCALE_FP=%d\n",
           BG_W, BG_H, BASE_W, BASE_H, PIPE_W, PIPE_H,
           BIRD_W, BIRD_H, GROUND_H, GAME_SCALE_FP);

    void *pc = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCreate");
    if (pc) {
        s32 pret = (s32)NC(G, pc, (u64)&g_audio_tid, 0,
                           (u64)(void*)audio_thread_entry, 0,
                           (u64)"flap_aud", 0);
        printf("audio thread: ret=%d tid=0x%llx\n",
               pret, (unsigned long long)g_audio_tid);
    } else {
        printf("audio thread: scePthreadCreate missing\n");
    }

    pad_prev = read_pad();

    u32 last_ms = now_ms();
    u32 prev_raw_logged = 0;

    while (!g_exit_now) {
        u32 cur_ms = now_ms();
        u32 dt_ms = cur_ms - last_ms;
        if (dt_ms > 50) dt_ms = 50;
        last_ms = cur_ms;
        float dt = (float)dt_ms / 1000.0f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        haptic_tick();
        audio_swap_tick();

        u32 raw = read_pad();
        if (raw != prev_raw_logged) {
            printf("PAD f=%u raw=%08x st=%d\n",
                   (unsigned)total_frames, raw, (int)game.state);
            prev_raw_logged = raw;
        }

        {
            static u32 max_dt_seen = 0;
            if (dt_ms > max_dt_seen) max_dt_seen = dt_ms;
            if (total_frames < 600 && (total_frames % 30) == 0) {
                printf("DBG f=%u st=%d y=%d vy=%d pipes=%d spd=%d sc=%d maxdt=%u\n",
                       (unsigned)total_frames, (int)game.state,
                       (int)(game.bird_y * 10.0f),
                       (int)game.bird_vy,
                       game.active_count,
                       (int)(game.pipe_speed * 100.0f),
                       game.score,
                       (unsigned)max_dt_seen);
                max_dt_seen = 0;
            }
        }

        u32 pressed = raw & ~pad_prev;
        pad_prev = raw;

        update_lightbar();

        render_clear_full(0xFF000000);
        apply_screen_viewport();

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
    return;
}
