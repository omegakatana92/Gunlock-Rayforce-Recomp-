/* Gunlock recomp - Taito F3 TC0630FDP video
 * Per-scanline compositor: 4 playfields with line scroll/zoom/colscroll, text (VRAM) and pixel (pivot)
 * layers, 4 sprite priority groups, clip planes, alpha blending, mosaic.
 * A C port of the behaviour documented in MAME's taito/taito_f3_v.cpp (BSD-3-Clause; Bryan McPhail,
 * ywy, 12Me21). The game uses MAME's 'extend' (1024x512 playfields) mode with sprite lag 2.
 */
#include "core.h"
#include <string.h>
#include <stdlib.h>
#include "state.h"

#define H_START 46
#define H_VIS   320
#define VIS_TOP 31
#define VIS_BOT 254
#define EXTEND  1
#define WIDTH_MASK 0x3ff

/* ------------------------------------------------------------------ sprite state (survives frames) */
typedef struct { int32_t code; uint8_t color, flip_x, flip_y, pri; int32_t x, y, scale_x, scale_y; } tsprite_t;
static tsprite_t spritelist[0x400];
static int sprite_count;
static uint16_t spr_fb[V_TOTAL][H_TOTAL];
static uint8_t flipscreen, sprite_bank, sprite_trails, sprite_pen_mask = 0x0f, sprite_extra_planes;

static uint32_t clut[0x2000];

static inline uint16_t lr(int word) { return be16(&g_lineram[(word & 0x7fff) * 2]); }

/* ------------------------------------------------------------------ line state */
typedef struct { int16_t l, r; } clip_t;
typedef struct {
    uint16_t mix_value; uint8_t prio, blend_mode, x_sample_enable, index;
} mix_t;
typedef struct { mix_t m; uint8_t blend_select_v; } spr_inf;
typedef struct { mix_t m; uint8_t pivot_control, blend_select_v; uint16_t pivot_enable; uint16_t reg_sx, reg_sy; } piv_inf;
typedef struct {
    mix_t m; uint16_t colscroll; uint8_t alt_tilemap;
    int32_t x_scale, y_scale; uint16_t pal_add; int32_t rowscroll;
    int32_t reg_sx, reg_sy, reg_fx_y, reg_fx_x;
} pf_inf;
typedef struct {
    int y;
    clip_t clip[4];
    uint8_t blend[4];
    uint8_t x_sample;
    uint8_t fx_6400;
    uint16_t bg_palette;
    piv_inf pivot;
    spr_inf sp[4];
    pf_inf pf[4];
} line_t;

static void set_mix(mix_t *m, uint16_t v) { m->mix_value = v; m->prio = v & 0xf; m->blend_mode = (v >> 14) & 3; }
static void set_prio(mix_t *m, uint8_t p) { m->mix_value = (m->mix_value & 0xfff0) | p; m->prio = p; }
static void set_blend(mix_t *m, uint8_t b) { m->mix_value = (m->mix_value & 0x3fff) | (b << 14); m->blend_mode = b; }
static void clip_upper(clip_t *c, int left, int right) { c->l = (int16_t)((c->l & 0xff) | (left << 8)); c->r = (int16_t)((c->r & 0xff) | (right << 8)); }
static void clip_lower(clip_t *c, int left, int right) { c->l = (int16_t)((c->l & 0x100) | left); c->r = (int16_t)((c->r & 0x100) | right); }

static int latched_addr(int section, int subsection, int y) {
    uint16_t latches = lr((section * 0x200) / 2 + y);
    int base = 0x4000 + 0x1000 * section + 0x200 * subsection;
    if ((latches >> (subsection + 4)) & 1) return (base + 0x800) / 2 + y;
    if ((latches >> subsection) & 1) return base / 2 + y;
    return 0;
}

static void read_line_ram(line_t *L, int y) {
    int w;
    for (int i = 2; i <= 3; i++) if ((w = latched_addr(0, i, y))) {
        uint16_t cs = lr(w);
        L->pf[i].colscroll = cs & 0x1ff;
        L->pf[i].alt_tilemap = !EXTEND && (cs & 0x200);
        clip_upper(&L->clip[2 * (i - 2) + 0], (cs >> 12) & 1, (cs >> 13) & 1);
        clip_upper(&L->clip[2 * (i - 2) + 1], (cs >> 14) & 1, (cs >> 15) & 1);
    }
    for (int i = 0; i < 4; i++) if ((w = latched_addr(1, i, y))) {
        uint16_t c = lr(w);
        clip_lower(&L->clip[i], c & 0xff, c >> 8);
    }
    if ((w = latched_addr(2, 0, y))) {
        uint16_t v = lr(w);
        L->pivot.blend_select_v = (v >> 9) & 1;
        L->pivot.pivot_control = v >> 8;
        for (int g = 0; g < 4; g++) set_blend(&L->sp[g].m, (v >> (g * 2)) & 3);
    }
    if ((w = latched_addr(2, 1, y))) {
        uint16_t v = lr(w);
        for (int i = 0; i < 4; i++) { int a = (v >> (4 * i)) & 0xf; int b = 0xf - a; L->blend[i] = (uint8_t)(b < 8 ? b : 8); }
    }
    if ((w = latched_addr(2, 2, y))) {
        uint16_t v = lr(w);
        L->x_sample = 16 - ((v >> 4) & 0xf);
        for (int p = 0; p < 4; p++) L->pf[p].m.x_sample_enable = (v >> p) & 1;
        for (int g = 0; g < 4; g++) L->sp[g].m.x_sample_enable = (v >> 8) & 1;
        L->pivot.m.x_sample_enable = (v >> 9) & 1;
        L->fx_6400 = (v & 0xfc00) >> 8;
    }
    if ((w = latched_addr(2, 3, y))) L->bg_palette = lr(w);
    if ((w = latched_addr(3, 0, y))) L->pivot.pivot_enable = lr(w);
    if ((w = latched_addr(3, 1, y))) set_mix(&L->pivot.m, lr(w));
    if ((w = latched_addr(3, 2, y))) {
        uint16_t v = lr(w);
        for (int g = 0; g < 4; g++) {
            set_mix(&L->sp[g].m, (L->sp[g].m.mix_value & 0xc00f) | ((v & 0x3ff) << 4));
            L->sp[g].blend_select_v = (v >> (12 + g)) & 1;
        }
    }
    if ((w = latched_addr(3, 3, y))) {
        uint16_t v = lr(w);
        for (int g = 0; g < 4; g++) set_prio(&L->sp[g].m, (v >> (g * 4)) & 0xf);
    }
    static const int FIX_Y[4] = {0, 3, 2, 1};
    for (int i = 0; i < 4; i++) if ((w = latched_addr(4, i, y))) {
        uint16_t v = lr(w);
        L->pf[i].x_scale = 256 - (v >> 8);
        L->pf[FIX_Y[i]].y_scale = (v & 0xff) << 1;
    }
    for (int i = 0; i < 4; i++) if ((w = latched_addr(5, i, y))) L->pf[i].pal_add = (uint16_t)(lr(w) * 16);
    for (int i = 0; i < 4; i++) if ((w = latched_addr(6, i, y))) {
        int32_t rs = (int32_t)lr(w) << 2;
        L->pf[i].rowscroll = (int32_t)((rs & 0xffffff00) - (rs & 0xff));
    }
    for (int i = 0; i < 4; i++) if ((w = latched_addr(7, i, y))) set_mix(&L->pf[i].m, lr(w));
}

static void get_pf_scroll(int pf, int32_t *rsx, int32_t *rsy) {
    int16_t sx_raw = (int16_t)g_ctrl0[pf];
    int16_t sy_raw = (int16_t)g_ctrl0[pf + 4];
    sy_raw = (int16_t)(sy_raw + (1 << 7));
    if (flipscreen) {
        sx_raw = (int16_t)(sx_raw + (320 << 6));
        sx_raw = (int16_t)(sx_raw + ((512 + 192) << 6));
        sy_raw = (int16_t)(-sy_raw);
    }
    sx_raw = (int16_t)(sx_raw + ((40 - 4 * pf) << 6));
    int32_t sx = (int32_t)sx_raw * 4;
    int32_t sy = (int32_t)sy_raw * 2;
    sx ^= 0xfc;
    sx = sx - (H_START << 8);
    if (flipscreen) sy = -sy;
    *rsx = sx; *rsy = sy;
}

/* ------------------------------------------------------------------ layer pixel sources */
/* playfield pixel (pixmap coordinates, flip applied as MAME's tilemap set_flip_all) */
static inline uint16_t pf_pixel(int tmap, int x, int y, uint8_t *flag) {
    if (flipscreen) { x = 1023 - x; y = 511 - y; }
    int col = x >> 4, row = y >> 4;
    const uint8_t *t = &g_pfram[tmap * 0x2000 + (row * 64 + col) * 4];
    uint16_t attr = be16(t), code = be16(t + 2);
    int px = x & 15, py = y & 15;
    if (attr & 0x4000) px = 15 - px;
    if (attr & 0x8000) py = 15 - py;
    uint16_t pal = attr & 0x1ff;
    uint8_t extra = (attr >> 10) & 3;
    uint8_t mask = (uint8_t)((((extra & ~pal) & 3) << 4) | 0x0f);
    uint8_t pen = g_tile_gfx[((uint32_t)code % g_tile_count) * 256 + py * 16 + px] & mask;
    *flag = pen ? (uint8_t)(0x10 | ((attr >> 9) & 1)) : 0;
    return (uint16_t)((pal << 4) + pen);
}
static inline uint8_t char_pen(const uint8_t *ram, int tile, int px, int py) {
    uint32_t row = be32(&ram[(tile * 32 + py * 4) & 0xffff]);
    return (row >> (4 * px)) & 0xf;
}
static inline uint16_t vram_pixel(int x, int y, uint8_t *flag) {      /* 512x512 text layer */
    if (flipscreen) { x = 511 - x; y = 511 - y; }
    int col = x >> 3, row = y >> 3;
    uint16_t w = be16(&g_textram[(row * 64 + col) * 2]);
    int px = x & 7, py = y & 7;
    if (w & 0x0100) px = 7 - px;
    if (w & 0x8000) py = 7 - py;
    uint8_t pen = char_pen(g_charram, w & 0xff, px, py);
    *flag = pen ? 0x10 : 0;
    return (uint16_t)((((w >> 9) & 0x3f) << 4) + pen);
}
static inline uint16_t pixel_pixel(int x, int y, uint8_t *flag) {     /* 512x256 pivot pixel layer */
    if (flipscreen) { x = 511 - x; y = 255 - y; }
    int col = x >> 3, row = y >> 3;
    int prow = row;
    int y_offs = row * 8 + g_ctrl1[5];
    if (flipscreen) y_offs += 0x100;
    if ((y_offs & 0x1ff) >= 256) prow += 32;
    uint16_t w = be16(&g_textram[((prow << 6) | col) * 2]);
    int px = x & 7, py = y & 7;
    if (w & 0x0100) px = 7 - px;
    if (w & 0x8000) py = 7 - py;
    uint8_t pen = char_pen(g_pivotram, col * 32 + row, px, py);
    *flag = pen ? 0x10 : 0;
    return (uint16_t)((((w >> 9) & 0x3f) << 4) + pen);
}

/* row usage (MAME skips layers whose source row holds no tiles) */
static uint8_t pf_row_used[4][32], text_row_used[64];
static void compute_usage(void) {
    memset(pf_row_used, 0, sizeof pf_row_used); memset(text_row_used, 0, sizeof text_row_used);
    for (int t = 0; t < 4; t++) for (int r = 0; r < 32; r++) for (int c = 0; c < 64; c++)
        if (be16(&g_pfram[t * 0x2000 + (r * 64 + c) * 4 + 2])) { pf_row_used[t][r] = 1; break; }
    for (int r = 0; r < 64; r++) for (int c = 0; c < 64; c++) if (g_textram[(r * 64 + c) * 2 + 1]) { text_row_used[r] = 1; break; }
}

/* ------------------------------------------------------------------ mixing */
typedef struct { uint16_t src_pal[H_TOTAL], dst_pal[H_TOTAL]; uint8_t src_blend[H_TOTAL], dst_blend[H_TOTAL]; } mixpix_t;
typedef struct { uint8_t src_prio[H_TOTAL], dst_prio[H_TOTAL], src_blendmode[H_TOTAL]; } primode_t;

enum { L_PIVOT, L_SPR, L_PF };
typedef struct { int kind, idx; mix_t *m; } layer_ref;

static int mosaic(int x, int sample) {
    int c = x - 46 + 114;
    if (c >= 432) c -= 432;
    return x - (c % sample);
}

static int calc_clip(const clip_t clip[4], const mix_t *m, clip_t *out) {
    const int16_t INF_L = H_START, INF_R = H_START + H_VIS;
    int en = (m->mix_value >> 8) & 0xf, inv = (m->mix_value >> 4) & 0xf;
    int normal = en & ~inv, invert = en & inv;
    if (!(m->mix_value & 0x1000)) { int t = normal; normal = invert; invert = t; }
    clip_t ranges[16]; int nr = 1;
    ranges[0].l = INF_L; ranges[0].r = INF_R;
    for (int p = 0; p < 4; p++) {
        int16_t cl = (int16_t)(clip[p].l - 1), cr = (int16_t)(clip[p].r - 2);
        if (normal & (1 << p)) {
            int k = 0;
            for (int i = 0; i < nr; i++) {
                if (cl > cr || ranges[i].r < cl || ranges[i].l > cr) continue;
                clip_t c = ranges[i];
                if (cl > c.l) c.l = cl;
                if (cr < c.r) c.r = cr;
                ranges[k++] = c;
            }
            nr = k;
        } else if ((invert & (1 << p)) && cl <= cr) {
            clip_t nw[16]; int nn = 0;
            for (int i = 0; i < nr && nn < 16; i++) { nw[nn].l = INF_L; nw[nn].r = cl; nn++; }
            for (int i = 0; i < nr && nn < 16; i++) { nw[nn].l = cr; nw[nn].r = INF_R; nn++; }
            int k = 0;
            for (int j = 0; j < nn; j++) {
                clip_t it = nw[j]; int erased = 0;
                for (int i = 0; i < nr; i++) {
                    if (ranges[i].l > it.l) it.l = ranges[i].l;
                    if (ranges[i].l > it.r) it.r = ranges[i].l;
                    if (it.l >= it.r) { erased = 1; break; }
                }
                if (!erased) nw[k++] = it;
            }
            memcpy(ranges, nw, sizeof(clip_t) * k); nr = k;
        }
    }
    memcpy(out, ranges, sizeof(clip_t) * nr);
    return nr;
}

static int layer_enable(const layer_ref *r) {
    const mix_t *m = r->m;
    if (r->kind == L_SPR) return (m->mix_value & 0x2000) && m->blend_mode != 0;
    return (m->mix_value & 0x2000) && m->blend_mode != 3;
}

static void mix_line(const layer_ref *lay, const line_t *L, mixpix_t *z, primode_t *pri, clip_t range) {
    const mix_t *g = lay->m;
    int kind = lay->kind;
    const pf_inf *pf = kind == L_PF ? &L->pf[lay->idx] : NULL;
    const piv_inf *pv = kind == L_PIVOT ? &L->pivot : NULL;
    int sy = 0; int use_pix = 0;
    if (pf) sy = ((pf->reg_fx_y >> 8) + pf->colscroll) & 0x1ff;
    if (pv) { use_pix = (pv->pivot_control & 0xa0) != 0; sy = (pv->reg_sy + L->y) & (use_pix ? 0xff : 0x1ff); }
    int sy_spr = L->y;
    uint8_t bsel_line = kind == L_SPR ? L->sp[lay->idx].blend_select_v : pv ? pv->blend_select_v : 0;
    int tmap = pf ? lay->idx : 0;
    for (int x = range.l; x < range.r; x++) {
        if (g->blend_mode == pri->src_blendmode[x]) continue;
        int real_x = g->x_sample_enable ? mosaic(x, L->x_sample) : x;
        uint16_t color; uint8_t flag = 0x10;
        if (kind == L_PF) {
            int gx = (((pf->reg_fx_x + (real_x - H_START) * pf->x_scale) >> 8) + H_START) & WIDTH_MASK;
            color = pf_pixel(tmap, gx, sy, &flag);
            if (!(flag & 0xf0)) continue;
        } else if (kind == L_PIVOT) {
            int gx = (real_x + pv->reg_sx) & 0x1ff;
            color = use_pix ? pixel_pixel(gx, sy, &flag) : vram_pixel(gx, sy, &flag);
            if (!(flag & 0xf0)) continue;
        } else {
            color = (sy_spr >= 0 && sy_spr < V_TOTAL && real_x >= 0 && real_x < H_TOTAL) ? spr_fb[sy_spr][real_x] : 0;
            if (((color >> 10) & 3) != lay->idx) continue;
        }
        uint8_t sel = kind == L_PF ? (flag & 1) : bsel_line;
        if (g->prio > pri->src_prio[x]) {
            if (color) {
                uint16_t pal = pf ? (uint16_t)(color + pf->pal_add) : color;
                switch (g->blend_mode) {
                case 1: sel = 2 + sel; /* fallthrough */
                case 2:
                    if (L->blend[sel] == 0) continue;
                    z->src_blend[x] = L->blend[sel];
                    break;
                default:
                    if (L->blend[sel] + L->blend[2 + sel] == 0) continue;
                    z->src_blend[x] = L->blend[2 + sel];
                    z->dst_blend[x] = L->blend[sel];
                    pri->dst_prio[x] = g->prio;
                    z->dst_pal[x] = pal;
                    break;
                }
                z->src_pal[x] = pal;
                pri->src_blendmode[x] = g->blend_mode;
                pri->src_prio[x] = g->prio;
            }
        } else if (g->prio >= pri->dst_prio[x]) {
            if (color) {
                uint16_t pal = pf ? (uint16_t)(color + pf->pal_add) : color;
                if (g->prio != pri->dst_prio[x]) z->dst_pal[x] = pal;
                else z->dst_pal[x] = 0;
                pri->dst_prio[x] = g->prio;
                if (pri->src_blendmode[x] == 1) z->dst_blend[x] = L->blend[sel];
                else z->dst_blend[x] = L->blend[2 + sel];
            }
        }
    }
}

static void render_line(uint32_t *dst, const mixpix_t *z) {
    for (int x = H_START; x < H_START + H_VIS; x++) {
        uint32_t s = clut[z->src_pal[x] & 0x1fff], d = clut[z->dst_pal[x] & 0x1fff];
        uint32_t r = (((s >> 16) & 0xff) * z->src_blend[x] + ((d >> 16) & 0xff) * z->dst_blend[x]) >> 3;
        uint32_t gg = (((s >> 8) & 0xff) * z->src_blend[x] + ((d >> 8) & 0xff) * z->dst_blend[x]) >> 3;
        uint32_t b = ((s & 0xff) * z->src_blend[x] + (d & 0xff) * z->dst_blend[x]) >> 3;
        if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
        dst[x - H_START] = (r << 16) | (gg << 8) | b;
    }
}

static int layer_used(const layer_ref *r, const line_t *L) {
    if (r->kind == L_SPR) return 1;
    if (r->kind == L_PIVOT) {
        int use_pix = (L->pivot.pivot_control & 0xa0) != 0;
        if (use_pix) return 1;
        int yi = (L->pivot.reg_sy + L->y) & 0x1ff;
        int ya = flipscreen ? 0x1ff - yi : yi;
        return text_row_used[ya >> 3];
    }
    const pf_inf *pf = &L->pf[r->idx];
    int yi = ((pf->reg_fx_y >> 8) + pf->colscroll) & 0x1ff;
    int ya = flipscreen ? 0x1ff - yi : yi;
    return pf_row_used[r->idx][ya >> 4];
}

static void scanline_draw(uint32_t *fb) {
    static line_t L;
    memset(&L, 0, sizeof L);
    L.x_sample = 16;
    for (int i = 0; i < 4; i++) { L.sp[i].m.index = (uint8_t)i; L.pf[i].x_scale = 0x80; L.pf[i].m.index = (uint8_t)i; }
    for (int p = 0; p < 4; p++) {
        get_pf_scroll(p, &L.pf[p].reg_sx, &L.pf[p].reg_sy);
        L.pf[p].reg_fx_y = L.pf[p].reg_sy;
    }
    if (flipscreen) { L.pivot.reg_sx = (uint16_t)(g_ctrl1[4] - 12); L.pivot.reg_sy = g_ctrl1[5]; }
    else { L.pivot.reg_sx = (uint16_t)(-g_ctrl1[4] - 5); L.pivot.reg_sy = (uint16_t)(-g_ctrl1[5]); }

    static mixpix_t z; static primode_t pri;
    for (int screen_y = 0; screen_y < 256; screen_y++) {
        int y = flipscreen ? 255 - screen_y : screen_y;
        read_line_ram(&L, y);
        L.y = screen_y;
        for (int p = 0; p < 4; p++) {
            pf_inf *pf = &L.pf[p];
            pf->reg_fx_x = pf->reg_sx + pf->rowscroll;
            pf->reg_fx_x += 10 * (pf->x_scale - (1 << 8));
        }
        if (screen_y >= VIS_TOP && screen_y <= VIS_BOT) {
            for (int x = 0; x < H_TOTAL; x++) {
                z.dst_pal[x] = L.bg_palette; z.dst_blend[x] = 8; z.src_pal[x] = 0; z.src_blend[x] = 0;
                pri.src_prio[x] = 0; pri.dst_prio[x] = 0; pri.src_blendmode[x] = 0xff;
            }
            layer_ref layers[9] = {
                {L_PIVOT, 0, &L.pivot.m},
                {L_SPR, 0, &L.sp[0].m}, {L_PF, 0, &L.pf[0].m},
                {L_SPR, 3, &L.sp[3].m}, {L_PF, 3, &L.pf[3].m},
                {L_SPR, 2, &L.sp[2].m}, {L_PF, 2, &L.pf[2].m},
                {L_SPR, 1, &L.sp[1].m}, {L_PF, 1, &L.pf[1].m},
            };
            /* stable sort by priority, descending (insertion sort is stable) */
            for (int i = 1; i < 9; i++) {
                layer_ref t = layers[i]; int j = i - 1;
                while (j >= 0 && layers[j].m->prio < t.m->prio) { layers[j + 1] = layers[j]; j--; }
                layers[j + 1] = t;
            }
            for (int i = 0; i < 9; i++) {
                if (!layer_enable(&layers[i]) || !layer_used(&layers[i], &L)) continue;
                clip_t ranges[16];
                int n = calc_clip(L.clip, layers[i].m, ranges);
                for (int k = 0; k < n; k++) mix_line(&layers[i], &L, &z, &pri, ranges[k]);
            }
            render_line(&fb[(screen_y - VIS_TOP) * SCREEN_W], &z);
        }
        if (screen_y != 0) for (int p = 0; p < 4; p++) L.pf[p].reg_fx_y += L.pf[p].y_scale;
    }
}

/* ------------------------------------------------------------------ sprites */
typedef struct { int32_t block_scale, pos, block_pos; int16_t global, subglobal; } axis_t;
static void axis_update(axis_t *a, uint8_t scroll, uint16_t posw, int multi, uint8_t block_ctrl, uint8_t new_zoom) {
    int16_t np = (int16_t)((posw & 0x800) ? (posw | 0xf000) : (posw & 0xfff));
    if (scroll & 1) a->subglobal = np;
    if (scroll & 2) a->global = np;
    if (!(scroll & 8)) {
        np = (int16_t)(np + a->global);
        if (!(scroll & 4)) np = (int16_t)(np + a->subglobal);
    }
    switch (block_ctrl) {
    case 0:
        if (!multi) { a->block_pos = np << 8; a->block_scale = 0x100 - new_zoom; }
        /* fallthrough */
    case 2: a->pos = a->block_pos; break;
    case 3: a->pos += a->block_scale * 16; break;
    }
}

static void get_sprite_info(void) {
    axis_t ax = {1 << 8, 0, 0, 0, 0}, ay = {1 << 8, 0, 0, 0, 0};
    uint8_t color = 0; int multi = 0;
    int n = 0, total = 0;
    for (int offs = 0; offs < 0x400 && total < 0x400; offs++) {
        total++;
        int bank = sprite_bank ? 0x4000 : 0;
        const uint8_t *sp = &g_spriteram[(bank + offs * 8) * 2];
        uint16_t spr[8];
        for (int i = 0; i < 8; i++) spr[i] = be16(sp + i * 2);
        if (spr[3] & 0x8000) {
            uint16_t c = spr[5];
            flipscreen = (c >> 13) & 1;
            sprite_extra_planes = (c >> 8) & 3;
            sprite_pen_mask = (uint8_t)((sprite_extra_planes << 4) | 0x0f);
            sprite_trails = (c >> 1) & 1;
            sprite_bank = c & 1;
        }
        if (spr[6] & 0x8000) {
            int no = spr[6] & 0x3ff;
            if (no == offs) break;
            offs = no - 1;
        }
        uint8_t cont = spr[4] >> 8;
        int lock = (cont >> 2) & 1;
        if (!lock) color = spr[4] & 0xff;
        uint8_t scroll = (spr[2] >> 12) & 0xf;
        uint16_t zooms = spr[1];
        axis_update(&ax, scroll, spr[2] & 0xfff, multi, (cont >> 6) & 3, zooms & 0xff);
        axis_update(&ay, scroll, spr[3] & 0xfff, multi, (cont >> 4) & 3, zooms >> 8);
        multi = (cont >> 3) & 1;
        int tile = spr[0] | ((spr[5] & 1) << 16);
        if (!tile) continue;
        int32_t tx = flipscreen ? (512 << 8) - ax.block_scale * 16 - ax.pos : ax.pos;
        int32_t ty = flipscreen ? (256 << 8) - ay.block_scale * 16 - ay.pos : ay.pos;
        if (tx + ax.block_scale * 16 <= (H_START << 8) || tx > ((H_START + H_VIS - 1) << 8) ||
            ty + ay.block_scale * 16 <= (VIS_TOP << 8) || ty > (VIS_BOT << 8)) continue;
        tsprite_t *s = &spritelist[n++];
        s->x = tx; s->y = ty;
        s->flip_x = flipscreen ? !(cont & 1) : (cont & 1);
        s->flip_y = flipscreen ? !((cont >> 1) & 1) : ((cont >> 1) & 1);
        s->code = tile; s->color = color;
        s->scale_x = ax.block_scale; s->scale_y = ay.block_scale;
        s->pri = (color >> 6) & 3;
    }
    sprite_count = n;
}

static void drawgfx(const tsprite_t *s) {
    const uint8_t *base = &g_spr_gfx[((uint32_t)s->code % g_spr_count) * 256];
    int fx = s->flip_x ? 15 : 0, fy = s->flip_y ? 15 : 0;
    int32_t dy8 = s->y;
    if (!flipscreen) dy8 += 255;
    for (int y = 0; y < 16; y++) {
        int dy = dy8 >> 8;
        dy8 += s->scale_y;
        if (dy < VIS_TOP || dy > VIS_BOT) continue;
        uint16_t *dest = spr_fb[dy];
        const uint8_t *src = &base[(y ^ fy) * 16];
        int32_t dx8 = s->x + 128;
        for (int x = 0; x < 16; x++) {
            int dx = dx8 >> 8;
            dx8 += s->scale_x;
            if (dx < H_START || dx > H_START + H_VIS - 1) continue;
            if (dx == (dx8 >> 8)) continue;
            uint8_t c = src[x ^ fx] & sprite_pen_mask;
            if (c && !dest[dx]) dest[dx] = (uint16_t)(0x1000 + ((s->color << 4) | c));
        }
    }
}
static void draw_sprites(void) {
    if (!sprite_trails) memset(spr_fb, 0, sizeof spr_fb);
    for (int i = sprite_count; i-- > 0; ) drawgfx(&spritelist[i]);
}

/* ------------------------------------------------------------------ frame */
void video_init(void) {}
void video_reset(void) {
    memset(spr_fb, 0, sizeof spr_fb); sprite_count = 0;
    flipscreen = sprite_bank = sprite_trails = 0; sprite_pen_mask = 0x0f; sprite_extra_planes = 0;
}
void video_frame(uint32_t *fb) {
    for (int i = 0; i < 0x2000; i++) clut[i] = be32(&g_palram[i * 4]) & 0xffffff;
    compute_usage();
    /* sprite_lag == 2 (gunlock): draw with last frame's sprite framebuffer, then render the
     * previously parsed list, then parse the current sprite RAM */
    scanline_draw(fb);
    draw_sprites();
    get_sprite_info();
}

void video_state(sstream_t *s) {
    SS(s, sprite_count);
    if (sprite_count < 0 || sprite_count > 0x400) { s->error = 1; return; }
    ss_io(s, spritelist, sizeof(tsprite_t) * (size_t)sprite_count);
    ss_io(s, spr_fb, sizeof spr_fb);
    SS(s, flipscreen); SS(s, sprite_bank); SS(s, sprite_trails); SS(s, sprite_pen_mask); SS(s, sprite_extra_planes);
}
