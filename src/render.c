#include "render.h"
#include "font_aa.h"
#include "assets.h"

extern const u8 asset_blob[];
extern const u8 asset_blob_end[];

static u32 *fbs[2];
static u32 *cur;
static int   active;

/* ---- Viewport transform (applied to every drawing call) ---- */
static float g_vscale = 1.0f;
static int   g_voffx  = 0;
static int   g_voffy  = 0;

void render_set_viewport(float scale, int offx, int offy) {
    g_vscale = scale;
    g_voffx  = offx;
    g_voffy  = offy;
}

int render_viewport_w(void) { return (int)(SCR_W * g_vscale + 0.5f); }
int render_viewport_h(void) { return (int)(SCR_H * g_vscale + 0.5f); }

void render_init(u32 *a, u32 *b) {
    fbs[0] = a; fbs[1] = b; cur = a; active = 0;
}

void render_begin_frame(void) { /* nothing */ }
void render_end_frame(void)   { /* nothing */ }
u32 *render_fb(void)          { return cur; }

void render_swap(void) {
    active ^= 1;
    cur = fbs[active];
}

void render_clear_full(u32 c) {
    u32 *p = cur;
    u64 n  = (u64)SCR_W * SCR_H;
    __asm__ volatile ("rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
}

void render_clear(u32 c) {
    int vx = g_voffx, vy = g_voffy;
    int vw = render_viewport_w();
    int vh = render_viewport_h();
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > SCR_W) vw = SCR_W - vx;
    if (vy + vh > SCR_H) vh = SCR_H - vy;
    if (vw <= 0 || vh <= 0) return;
    for (int y = 0; y < vh; y++) {
        u32 *p = cur + (vy + y) * SCR_W + vx;
        u64 n  = (u64)vw;
        __asm__ volatile ("rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    }
}

void render_fill_rect(int x, int y, int w, int h, u32 c) {
    int vx = (int)(x * g_vscale + g_voffx + 0.5f);
    int vy = (int)(y * g_vscale + g_voffy + 0.5f);
    int vw = (int)(w * g_vscale + 0.5f);
    int vh = (int)(h * g_vscale + 0.5f);

    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > SCR_W) vw = SCR_W - vx;
    if (vy + vh > SCR_H) vh = SCR_H - vy;
    if (vw <= 0 || vh <= 0) return;

    for (int j = 0; j < vh; j++) {
        u32 *p = cur + (vy + j) * SCR_W + vx;
        u64 n  = (u64)vw;
        __asm__ volatile ("rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    }
}

static const struct asset *get_asset(int id) {
    if (id < 0 || id >= ASSET_COUNT) return 0;
    return &asset_table[id];
}

/* -----------------------------------------------------------------
   SOURCE-MAJOR scaled blit.
   For every source pixel (sx, sy), the destination region it fills is
     [ sx*dst_w/src_w , (sx+1)*dst_w/src_w )   x
     [ sy*dst_h/src_h , (sy+1)*dst_h/src_h )
   That's ONE division per source pixel instead of one per destination
   pixel — ~25x fewer divisions per frame.  This is the fix for the
   FPS drop: previously the dst-major loop did ~4M divisions/frame.
-------------------------------------------------------------------- */
static void blit_indexed_fp(const u8 *pal, const u8 *idx,
                            int src_w, int src_h,
                            int dst_x, int dst_y,
                            int dst_w, int dst_h,
                            u8 alpha, int force_opaque)
{
    if (dst_w < 1 || dst_h < 1) return;

    /* Clip against the viewport in framebuffer coords */
    int vpx0 = g_voffx;
    int vpy0 = g_voffy;
    int vpx1 = g_voffx + render_viewport_w();
    int vpy1 = g_voffy + render_viewport_h();
    if (vpx0 < 0) vpx0 = 0;
    if (vpy0 < 0) vpy0 = 0;
    if (vpx1 > SCR_W) vpx1 = SCR_W;
    if (vpy1 > SCR_H) vpy1 = SCR_H;

    /* Early out: entire destination off-screen */
    if (dst_x >= vpx1 || dst_x + dst_w <= vpx0) return;
    if (dst_y >= vpy1 || dst_y + dst_h <= vpy0) return;

    /* Clip in destination-local coords */
    int cx0 = 0, cy0 = 0, cx1 = dst_w, cy1 = dst_h;
    if (dst_x + cx0 < vpx0) cx0 = vpx0 - dst_x;
    if (dst_y + cy0 < vpy0) cy0 = vpy0 - dst_y;
    if (dst_x + cx1 > vpx1) cx1 = vpx1 - dst_x;
    if (dst_y + cy1 > vpy1) cy1 = vpy1 - dst_y;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    /* Narrow the source rows we iterate: only those whose destination
       rows intersect [cy0, cy1).  Same trick as the columns. */
    int sy_start = (cy0 * src_h) / dst_h;
    int sy_end   = ((cy1 - 1) * src_h) / dst_h + 1;
    if (sy_start < 0) sy_start = 0;
    if (sy_end > src_h) sy_end = src_h;

    for (int sy = sy_start; sy < sy_end; sy++) {
        int dy0 = (sy * dst_h) / src_h;
        int dy1 = ((sy + 1) * dst_h) / src_h;
        if (dy1 <= dy0) dy1 = dy0 + 1;

        if (dy0 < cy0) dy0 = cy0;
        if (dy1 > cy1) dy1 = cy1;
        if (dy1 <= dy0) continue;

        const u8 *srow = idx + sy * src_w;
        int py0 = dst_y + dy0;
        int py1 = dst_y + dy1;

        int dx_prev = 0;
        for (int sx = 0; sx < src_w; sx++) {
            int dx_next = ((sx + 1) * dst_w) / src_w;
            if (dx_next <= dx_prev) dx_next = dx_prev + 1;

            int dx0 = dx_prev;
            int dx1 = dx_next;
            dx_prev = dx_next;

            if (dx0 < cx0) dx0 = cx0;
            if (dx1 > cx1) dx1 = cx1;
            if (dx1 <= dx0) continue;

            u8  i  = srow[sx];
            u32 sa = pal[i*4+3];
            if (sa == 0) continue;

            u8 ea;
            if (force_opaque) {
                ea = 255;
            } else {
                ea = (u8)((sa * alpha) >> 8);
                if (ea == 0) continue;
            }
            u32 col = ((u32)ea << 24) | (pal[i*4+0] << 16)
                                       | (pal[i*4+1] << 8) | pal[i*4+2];

            int px0 = dst_x + dx0;
            int span = dx1 - dx0;

            for (int py = py0; py < py1; py++) {
                u32 *row = cur + py * SCR_W + px0;
                for (int k = 0; k < span; k++) row[k] = col;
            }
        }
    }
}

void render_blit_scaled_fp(int id, float xf, float yf, int scale_fp, u8 alpha) {
    const struct asset *a = get_asset(id);
    if (!a || a->fmt != ASSET_FMT_RGBA8_INDEXED) return;
    if (scale_fp < 1) scale_fp = 1;

    int eff_scale_fp = (int)(scale_fp * g_vscale + 0.5f);
    if (eff_scale_fp < 1) eff_scale_fp = 1;

    int x0 = (int)(xf * g_vscale + g_voffx + 0.5f);
    int y0 = (int)(yf * g_vscale + g_voffy + 0.5f);
    int dst_w = ((int)a->w * eff_scale_fp + 128) >> 8;
    int dst_h = ((int)a->h * eff_scale_fp + 128) >> 8;

    const u8 *pal = asset_blob + a->offset;
    const u8 *idx = pal + 256 * 4;

    blit_indexed_fp(pal, idx, (int)a->w, (int)a->h,
                    x0, y0, dst_w, dst_h, alpha, 0);
}

void render_blit_scaled_bg_fp(int id, float xf, int scale_fp) {
    const struct asset *a = get_asset(id);
    if (!a || a->fmt != ASSET_FMT_RGBA8_INDEXED) return;
    if (scale_fp < 1) scale_fp = 1;

    int eff_scale_fp = (int)(scale_fp * g_vscale + 0.5f);
    if (eff_scale_fp < 1) eff_scale_fp = 1;

    int x0 = (int)(xf * g_vscale + g_voffx + 0.5f);
    int dst_w = ((int)a->w * eff_scale_fp + 128) >> 8;
    int dst_h = ((int)a->h * eff_scale_fp + 128) >> 8;

    const u8 *pal = asset_blob + a->offset;
    const u8 *idx = pal + 256 * 4;

    blit_indexed_fp(pal, idx, (int)a->w, (int)a->h,
                    x0, g_voffy, dst_w, dst_h, 255, 1);
}

/* ---- Legacy integer blits (kept for compatibility) ---- */
void render_blit_scaled(int id, float xf, float yf, float scl, u8 alpha) {
    render_blit_scaled_fp(id, xf, yf, (int)(scl * 256.0f + 0.5f), alpha);
}

void render_blit_scaled_bg(int id, float xf, float scl) {
    render_blit_scaled_bg_fp(id, xf, (int)(scl * 256.0f + 0.5f));
}

void render_blit_tiled(int id, float scroll_x, float scale) {
    const struct asset *a = get_asset(id);
    if (!a) return;
    int tile_w = (int)(a->w * scale * g_vscale);
    if (tile_w <= 0) return;
    int base_x = (int)scroll_x;
    base_x = base_x % tile_w;
    if (base_x > 0) base_x -= tile_w;
    for (int x = base_x; x < SCR_W; x += tile_w)
        render_blit_scaled(id, (float)x, 0.0f, scale, 255);
}

/* ---- Anti-aliased text ---- */

void render_text(int x, int y, const char *s, u32 c, int scale) {
    int sx = (int)(x * g_vscale + g_voffx + 0.5f);
    int sy = (int)(y * g_vscale + g_voffy + 0.5f);
    int sh = (int)(scale * 8 * g_vscale + 0.5f);
    if (sh < 4) sh = 4;
    font_aa_draw(cur, sx, sy, s, c, sh);
}

int render_text_width(const char *s, int scale) {
    int sh = (int)(scale * 8 * g_vscale + 0.5f);
    if (sh < 4) sh = 4;
    return font_aa_width(s, sh);
}

void render_text_center(int y, const char *s, u32 c, int scale) {
    int sh = (int)(scale * 8 * g_vscale + 0.5f);
    if (sh < 4) sh = 4;
    int w = font_aa_width(s, sh);
    int sy = (int)(y * g_vscale + g_voffy + 0.5f);
    int vp_w = render_viewport_w();
    font_aa_draw(cur, g_voffx + (vp_w - w) / 2, sy, s, c, sh);
}
