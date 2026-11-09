/* Gunlock recomp - Taito F3 main board: memory map, I/O, interrupts, EEPROM, frame scheduling,
 * ROM loading and save-state container. */
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"
#include "state.h"
#ifdef _WIN32
#include <windows.h>
/* paths are UTF-8 everywhere; Windows needs the wide-char API for non-ASCII folders */
static FILE *u8fopen(const char *path, const char *mode) {
    wchar_t wp[4096], wm[16];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 4096) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16)) return NULL;
    return _wfopen(wp, wm);
}
#define fopen u8fopen
#endif

uint8_t g_rom[0x200000];
uint8_t g_ram[0x20000];
uint8_t g_palram[0x8000];
uint8_t g_spriteram[0x10000];
uint8_t g_pfram[0xc000];
uint8_t g_textram[0x2000];
uint8_t g_charram[0x2000];
uint8_t g_lineram[0x10000];
uint8_t g_pivotram[0x10000];
uint16_t g_ctrl0[8], g_ctrl1[8];
uint8_t g_dpram[0x800];
uint16_t g_input[2];
uint8_t *g_spr_gfx;  uint32_t g_spr_count;
uint8_t *g_tile_gfx; uint32_t g_tile_count;
static uint8_t *g_snd_prog, *g_snd_samples;

int64_t  g_cycles;
int64_t  g_next_event;
int      g_frame_done;
uint64_t g_frame_count;

static int irq3_done;
static int64_t ev_snd;
static uint16_t coin_word[2];
static uint32_t timer_ctrl;
#define SND_SLICE 4096
static uint32_t fb[SCREEN_W * SCREEN_H];

void cpu_take_pending_irq(void);  /* cpu.c */

/* ------------------------------------------------------------------ scheduling */
void core_irq_changed(void) { g_next_event = 0; }

static void compute_next_event(void) {
    int64_t n = ev_snd;
    if (!irq3_done && IRQ3_DELAY < n) n = IRQ3_DELAY;
    if (CYCLES_PER_FRAME < n) n = CYCLES_PER_FRAME;
    g_next_event = n;
}

void core_service(void) {
    if (g_cycles >= ev_snd) { snd_run_until(g_cycles); while (ev_snd <= g_cycles) ev_snd += SND_SLICE; }
    if (!irq3_done && g_cycles >= IRQ3_DELAY) { irq3_done = 1; cpu_set_irq_line(3); }
    if (g_cycles >= CYCLES_PER_FRAME) g_frame_done = 1;
    compute_next_event();
    if (!g_frame_done) cpu_take_pending_irq();
}

/* ------------------------------------------------------------------ I/O (TC0640FIO) */
static uint32_t control_r(int offset) {
    uint16_t p0 = g_input[0], p1 = g_input[1];
    switch (offset) {
    case 0: {
        uint32_t v = 0xffff;
        if (p0 & IN_B1) v &= ~0x0001; if (p0 & IN_B2) v &= ~0x0002;
        if (p0 & IN_B3) v &= ~0x0004; if (p0 & IN_B4) v &= ~0x0008;
        if (p1 & IN_B1) v &= ~0x0010; if (p1 & IN_B2) v &= ~0x0020;
        if (p1 & IN_B3) v &= ~0x0040; if (p1 & IN_B4) v &= ~0x0080;
        if ((p0 | p1) & IN_TILT) v &= ~0x0100;
        if ((p0 | p1) & IN_SERVICE) v &= ~0x0200;
        if (p0 & IN_START) v &= ~0x1000; if (p1 & IN_START) v &= ~0x2000;
        uint32_t e = 0xfc | (eeprom_do() ? 1 : 0);   /* EEPROMIN: DO, test (active low), coins (active low) */
        if ((p0 | p1) & IN_TEST) e &= ~0x02; else e |= 0x02;
        if (p0 & IN_COIN) e &= ~0x10;
        if (p1 & IN_COIN) e &= ~0x20;
        return v | (e << 16) | (e << 24);
    }
    case 1: {
        uint32_t v = 0xffff;
        if (p0 & IN_UP) v &= ~0x01; if (p0 & IN_DOWN) v &= ~0x02; if (p0 & IN_LEFT) v &= ~0x04; if (p0 & IN_RIGHT) v &= ~0x08;
        if (p1 & IN_UP) v &= ~0x10; if (p1 & IN_DOWN) v &= ~0x20; if (p1 & IN_LEFT) v &= ~0x40; if (p1 & IN_RIGHT) v &= ~0x80;
        return v | ((uint32_t)coin_word[0] << 16);
    }
    case 2: case 3: return 0xffff0000u;          /* analog dials (unused): MSW active-low unknown */
    case 4: return 0xffffffffu;
    case 5: return 0x0000ffffu | ((uint32_t)coin_word[1] << 16);
    }
    return 0xffffffffu;
}
static void control_w(int offset, uint32_t d, uint32_t mask) {
    switch (offset) {
    case 0: return;                                  /* watchdog */
    case 1: if (mask & 0xff000000u) coin_word[0] = (uint16_t)(d >> 16); return;
    case 4: if (mask & 0xff) eeprom_write_lines((d >> 2) & 1, (d >> 4) & 1, (d >> 3) & 1); return;
    case 5: if (mask & 0xff000000u) coin_word[1] = (uint16_t)(d >> 16); return;
    }
}

/* ------------------------------------------------------------------ bus */
static inline uint32_t arr_r32(const uint8_t *base, uint32_t off) { return be32(base + off); }
static inline void arr_w32(uint8_t *base, uint32_t off, uint32_t d, uint32_t mask) {
    for (int i = 0; i < 4; i++) {
        int sh = 24 - i * 8;
        if ((mask >> sh) & 0xff) base[off + i] = (uint8_t)(d >> sh);
    }
}
static int64_t snd_sync_point(void) { return g_cycles; }

/* aligned 32-bit device access */
static uint32_t dev_r32(uint32_t a) {
    a &= 0xfffffc;
    if (a < 0x200000) return arr_r32(g_rom, a);
    if ((a & 0xfc0000) == 0x400000) return arr_r32(g_ram, a & 0x1ffff);
    if (a >= 0x440000 && a < 0x448000) return arr_r32(g_palram, a - 0x440000);
    if (a >= 0x4a0000 && a < 0x4a0020) return control_r((a - 0x4a0000) >> 2);
    if (a >= 0x600000 && a < 0x610000) return arr_r32(g_spriteram, a - 0x600000);
    if (a >= 0x610000 && a < 0x61c000) return arr_r32(g_pfram, a - 0x610000);
    if (a >= 0x61c000 && a < 0x61e000) return arr_r32(g_textram, a - 0x61c000);
    if (a >= 0x61e000 && a < 0x620000) return arr_r32(g_charram, a - 0x61e000);
    if (a >= 0x620000 && a < 0x630000) return arr_r32(g_lineram, a - 0x620000);
    if (a >= 0x630000 && a < 0x640000) return arr_r32(g_pivotram, a - 0x630000);
    if (a >= 0xc00000 && a < 0xc00800) { snd_run_until(snd_sync_point()); return arr_r32(g_dpram, a - 0xc00000); }
    return 0xffffffffu;
}
static void dev_w32(uint32_t a, uint32_t d, uint32_t mask) {
    a &= 0xfffffc;
    if ((a & 0xfc0000) == 0x400000) { arr_w32(g_ram, a & 0x1ffff, d, mask); return; }
    if (a >= 0x440000 && a < 0x448000) { arr_w32(g_palram, a - 0x440000, d, mask); return; }
    if (a >= 0x4a0000 && a < 0x4a0020) { control_w((a - 0x4a0000) >> 2, d, mask); return; }
    if (a >= 0x4c0000 && a < 0x4c0004) { timer_ctrl = (timer_ctrl & ~mask) | (d & mask); return; }
    if (a >= 0x600000 && a < 0x610000) { arr_w32(g_spriteram, a - 0x600000, d, mask); return; }
    if (a >= 0x610000 && a < 0x61c000) { arr_w32(g_pfram, a - 0x610000, d, mask); return; }
    if (a >= 0x61c000 && a < 0x61e000) { arr_w32(g_textram, a - 0x61c000, d, mask); return; }
    if (a >= 0x61e000 && a < 0x620000) { arr_w32(g_charram, a - 0x61e000, d, mask); return; }
    if (a >= 0x620000 && a < 0x630000) { arr_w32(g_lineram, a - 0x620000, d, mask); return; }
    if (a >= 0x630000 && a < 0x640000) { arr_w32(g_pivotram, a - 0x630000, d, mask); return; }
    if (a >= 0x660000 && a < 0x660020) {
        uint16_t *r = a < 0x660010 ? &g_ctrl0[(a & 0xf) >> 1] : &g_ctrl1[(a & 0xf) >> 1];
        uint16_t hm = (uint16_t)(mask >> 16), lm = (uint16_t)mask;
        r[0] = (r[0] & ~hm) | ((uint16_t)(d >> 16) & hm);
        r[1] = (r[1] & ~lm) | ((uint16_t)d & lm);
        return;
    }
    if (a >= 0xc00000 && a < 0xc00800) { snd_run_until(snd_sync_point()); arr_w32(g_dpram, a - 0xc00000, d, mask); return; }
    if (a == 0xc80000) { snd_run_until(snd_sync_point()); snd_set_reset_line(0); return; }
    if (a == 0xc80100) { snd_run_until(snd_sync_point()); snd_set_reset_line(1); return; }
    /* 0x300000 sound bankswitch: only used by Kirameki; ignored */
}

uint32_t mem_read8(uint32_t a) {
    a &= 0xffffff;
    return (dev_r32(a) >> (24 - (a & 3) * 8)) & 0xff;
}
uint32_t mem_read16(uint32_t a) {
    a &= 0xffffff;
    if (a & 1) return (mem_read8(a) << 8) | mem_read8(a + 1);
    return (dev_r32(a) >> (16 - (a & 2) * 8)) & 0xffff;
}
uint32_t mem_read32(uint32_t a) {
    a &= 0xffffff;
    if (a & 3) return (mem_read16(a) << 16) | mem_read16(a + 2);
    return dev_r32(a);
}
void mem_write8(uint32_t a, uint32_t v) {
    a &= 0xffffff;
    int sh = 24 - (a & 3) * 8;
    dev_w32(a, (v & 0xff) << sh, 0xffu << sh);
}
void mem_write16(uint32_t a, uint32_t v) {
    a &= 0xffffff;
    if (a & 1) { mem_write8(a, v >> 8); mem_write8(a + 1, v); return; }
    int sh = 16 - (a & 2) * 8;
    dev_w32(a, (v & 0xffff) << sh, 0xffffu << sh);
}
void mem_write32(uint32_t a, uint32_t v) {
    a &= 0xffffff;
    if (a & 3) { mem_write16(a, v >> 16); mem_write16(a + 2, v & 0xffff); return; }
    dev_w32(a, v, 0xffffffffu);
}

/* ------------------------------------------------------------------ EEPROM 93C46 (16-bit) */
uint16_t g_eeprom[64];
enum { EE_RESET, EE_START, EE_CMD, EE_READ, EE_DATA, EE_DONE };
static int ee_state, ee_cs, ee_clk, ee_di, ee_bits, ee_accum, ee_locked = 1, ee_cmd, ee_addr;
static uint32_t ee_shift;

void eeprom_reset(void) { ee_state = EE_RESET; ee_cs = ee_clk = ee_di = 0; ee_locked = 1; }
int eeprom_do(void) {
    if (ee_state == EE_READ) return (ee_shift >> 31) & 1;
    return 1;
}
static void ee_event(int ev) { /* 0=cs rise 1=cs fall 2=clk rise */
    if (ev == 1) { ee_state = EE_RESET; return; }
    switch (ee_state) {
    case EE_RESET: if (ev == 0) ee_state = EE_START; break;
    case EE_START: if (ev == 2 && ee_di) { ee_accum = ee_bits = 0; ee_state = EE_CMD; } break;
    case EE_CMD:
        if (ev == 2) {
            ee_accum = (ee_accum << 1) | ee_di;
            if (++ee_bits == 8) {
                ee_bits = 0; ee_addr = ee_accum & 0x3f;
                switch (ee_accum >> 6) {
                case 0:
                    switch (ee_addr >> 4) {
                    case 0: ee_locked = 1; ee_state = EE_RESET; break;
                    case 1: ee_cmd = 2; ee_shift = 0; ee_state = EE_DATA; break; /* WRAL */
                    case 2: if (!ee_locked) for (int i = 0; i < 64; i++) g_eeprom[i] = 0xffff;
                            ee_state = ee_locked ? EE_RESET : EE_DONE; break;
                    case 3: ee_locked = 0; ee_state = EE_RESET; break;
                    }
                    break;
                case 1: ee_cmd = 1; ee_shift = 0; ee_state = EE_DATA; break;
                case 2: ee_shift = 0; ee_state = EE_READ; break;
                case 3: if (!ee_locked) g_eeprom[ee_addr] = 0xffff; ee_state = ee_locked ? EE_RESET : EE_DONE; break;
                }
            }
        }
        break;
    case EE_READ:
        if (ev == 2) {
            int bi = ee_bits++;
            if (bi == 0) ee_shift = (uint32_t)g_eeprom[ee_addr & 63] << 16;
            else ee_shift = (ee_shift << 1) | 1;
        }
        break;
    case EE_DATA:
        if (ev == 2) {
            ee_shift = (ee_shift << 1) | ee_di;
            if (++ee_bits == 16) {
                if (ee_locked) { ee_state = EE_RESET; break; }
                if (ee_cmd == 1) g_eeprom[ee_addr] = (uint16_t)ee_shift;
                else for (int i = 0; i < 64; i++) g_eeprom[i] &= (uint16_t)ee_shift;
                ee_state = EE_DONE;
            }
        }
        break;
    }
}
void eeprom_write_lines(int di, int cs, int clk) {
    ee_di = di;
    if (cs != ee_cs) { ee_cs = cs; ee_event(cs ? 0 : 1); if (cs) { ee_clk = clk; return; } }
    if (clk != ee_clk) { ee_clk = clk; if (clk) ee_event(2); }
}
void core_eeprom_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t b[128];
    if (fread(b, 1, 128, f) == 128) for (int i = 0; i < 64; i++) g_eeprom[i] = (b[i * 2] << 8) | b[i * 2 + 1];
    fclose(f);
}
void core_eeprom_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t b[128];
    for (int i = 0; i < 64; i++) { b[i * 2] = g_eeprom[i] >> 8; b[i * 2 + 1] = (uint8_t)g_eeprom[i]; }
    fwrite(b, 1, 128, f); fclose(f);
}
/* a blank 93C46 reads all ones; the game initialises it on first boot (as on a new board) */
void core_eeprom_factory(void) { for (int i = 0; i < 64; i++) g_eeprom[i] = 0xffff; }

/* ------------------------------------------------------------------ ROM loading */
typedef struct { const char *name; uint32_t size; uint32_t crc; uint8_t *data; } romfile_t;
/* MAME 'gunlock' (Ver 2.3O 1994/01/20). CRCs from MAME's taito_f3.cpp */
static romfile_t roms[] = {
    {"d66-18.ic24", 0x40000, 0x8418513e}, {"d66-19.ic26", 0x40000, 0x95731473},
    {"d66-21.ic37", 0x40000, 0xbd0d60f2}, {"d66-24.ic35", 0x40000, 0x97816378},
    {"d66-03.ic45", 0x100000, 0xe7a4a491}, {"d66-04.ic46", 0x100000, 0xc1c7aaa7}, {"d66-05.ic47", 0x100000, 0xa3cefe04},
    {"d66-06.ic48", 0x100000, 0xb3d8126d}, {"d66-07.ic49", 0x100000, 0xa6da9be7}, {"d66-08.ic50", 0x100000, 0x9959f30b},
    {"d66-23.ic10", 0x40000, 0x57fb7c49}, {"d66-22.ic23", 0x40000, 0x83dd7f9b},
    {"d66-01.ic2", 0x200000, 0x58c92efa}, {"d66-02.ic3", 0x200000, 0xdcdafaab},
};
#define NROMS (int)(sizeof(roms) / sizeof(roms[0]))
/* known relatives that are NOT supported by this build (different program code) */
static const struct { uint32_t crc; const char *what; } other_sets[] = {
    {0xc6a89434, "Gunlock Ver 2.0O (MAME 'gunlocko')"},
};

static char last_error[512];
static uint32_t rom_fp;

static void set_err(const char *fmt, const char *a) { if (!last_error[0]) snprintf(last_error, sizeof last_error, fmt, a); }

static int load_from_zip(const char *path) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, path, 0)) { set_err("Cannot open %s as a zip archive.", path); return 0; }
    int ok = 1;
    for (int i = 0; i < NROMS && ok; i++) {
        int idx = -1;
        /* locate by CRC first (renamed files are fine), then by name */
        int n = (int)mz_zip_reader_get_num_files(&z);
        for (int k = 0; k < n && idx < 0; k++) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&z, k, &st) && st.m_crc32 == roms[i].crc && st.m_uncomp_size == roms[i].size) idx = k;
        }
        if (idx < 0) idx = mz_zip_reader_locate_file(&z, roms[i].name, NULL, MZ_ZIP_FLAG_IGNORE_PATH);
        if (idx < 0) {
            for (int k = 0; k < n; k++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&z, k, &st)) continue;
                for (size_t o = 0; o < sizeof other_sets / sizeof other_sets[0]; o++)
                    if (st.m_crc32 == other_sets[o].crc) { set_err("This archive is %s, which has different program code. This release supports the MAME 'gunlock' set (Ver 2.3O 1994/01/20) only.", other_sets[o].what); }
            }
            set_err("Missing ROM %s. This is not a complete MAME 'gunlock' set.", roms[i].name);
            ok = 0; break;
        }
        size_t sz; void *p = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
        if (!p || sz != roms[i].size) { set_err("%s has the wrong size (bad dump or a different version).", roms[i].name); ok = 0; free(p); break; }
        if (mz_crc32(MZ_CRC32_INIT, p, sz) != roms[i].crc) {
            set_err("%s has a bad checksum (corrupt or a different revision).", roms[i].name); ok = 0; free(p); break;
        }
        roms[i].data = p;
    }
    mz_zip_reader_end(&z);
    return ok;
}
static int load_from_dir(const char *dir) {
    int ok = 1;
    for (int i = 0; i < NROMS && ok; i++) {
        char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, roms[i].name);
        FILE *f = fopen(p, "rb");
        if (!f) { set_err("Missing file %s in the folder.", roms[i].name); ok = 0; break; }
        roms[i].data = malloc(roms[i].size);
        if (fread(roms[i].data, 1, roms[i].size, f) != roms[i].size) { set_err("%s has the wrong size.", roms[i].name); ok = 0; }
        else if (mz_crc32(MZ_CRC32_INIT, roms[i].data, roms[i].size) != roms[i].crc) { set_err("%s has a bad checksum.", roms[i].name); ok = 0; }
        fclose(f);
    }
    return ok;
}

/* 16x16 tiles, low 4bpp "packed lsb" (8 bytes per row, low nibble first) + hi 2 planes */
static uint8_t *decode_16x16(const uint8_t *lo, const uint8_t *hi, uint32_t count, int sprite) {
    uint8_t *dst = malloc((size_t)count * 256), *d = dst;
    for (uint32_t t = 0; t < count; t++) {
        const uint8_t *L = lo + (size_t)t * 128, *H = hi + (size_t)t * 64;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            uint8_t b = L[y * 8 + (x >> 1)];
            int v = (x & 1) ? (b >> 4) : (b & 15);
            int h;
            if (sprite) { /* layout_6bpp_sprite_hi */
                h = (H[y * 4 + (x >> 2)] >> (2 * (x & 3))) & 3;
            } else {      /* layout_6bpp_tile_hi: plane offsets {8,0} */
                int xb = x & 7, base = y * 4 + (x >> 3) * 2;
                h = ((H[base] >> xb) & 1) | (((H[base + 1] >> xb) & 1) << 1);
            }
            *d++ = (uint8_t)(v | (h << 4));
        }
    }
    return dst;
}

const char *core_last_error(void) { return last_error; }
const char *core_romset_name(void) { return "gunlock"; }
uint32_t core_rom_fingerprint(void) { return rom_fp; }

int core_load_romset(const char *path) {
    size_t l = strlen(path);
    last_error[0] = 0;
    int isdir = !(l > 4 && (!strcmp(path + l - 4, ".zip") || !strcmp(path + l - 4, ".ZIP") || !strcmp(path + l - 4, ".Zip")));
    int ok = isdir ? load_from_dir(path) : load_from_zip(path);
    if (!ok) {
        for (int i = 0; i < NROMS; i++) { free(roms[i].data); roms[i].data = NULL; }
        if (!last_error[0]) snprintf(last_error, sizeof last_error, "Not a valid 'gunlock' romset.");
        return 0;
    }
    rom_fp = 0;
    for (int i = 0; i < NROMS; i++) rom_fp = (rom_fp ^ roms[i].crc) * 16777619u;
    free(g_spr_gfx); free(g_tile_gfx); free(g_snd_prog); free(g_snd_samples);
    /* program: ROM_LOAD32_BYTE x4 */
    memset(g_rom, 0, sizeof g_rom);
    for (int i = 0; i < 0x40000; i++) for (int k = 0; k < 4; k++) g_rom[i * 4 + k] = roms[k].data[i];
    /* sprites: d66-03/04 LOAD16_BYTE (2MB), d66-05 hi planes */
    uint8_t *lo = malloc(0x200000);
    for (int i = 0; i < 0x100000; i++) { lo[i * 2] = roms[4].data[i]; lo[i * 2 + 1] = roms[5].data[i]; }
    g_spr_count = 0x200000 / 128;
    g_spr_gfx = decode_16x16(lo, roms[6].data, g_spr_count, 1);
    /* tilemaps: d66-06/07 LOAD32_WORD, d66-08 hi planes */
    for (int i = 0; i < 0x100000; i += 2) {
        lo[i * 2 + 0] = roms[7 - 0].data[i]; lo[i * 2 + 1] = roms[7].data[i + 1];
        lo[i * 2 + 2] = roms[8].data[i]; lo[i * 2 + 3] = roms[8].data[i + 1];
    }
    g_tile_count = 0x200000 / 128;
    g_tile_gfx = decode_16x16(lo, roms[9].data, g_tile_count, 0);
    free(lo);
    /* sound program: d66-23 even, d66-22 odd (512KB at 0xc00000 on the sound CPU) */
    g_snd_prog = malloc(0x80000);
    for (int i = 0; i < 0x40000; i++) { g_snd_prog[i * 2] = roms[10].data[i]; g_snd_prog[i * 2 + 1] = roms[11].data[i]; }
    /* ES5505 samples: 16-bit words, ROM byte in the high half; d66-01 at 0, d66-02 at 0x400000 (bytes) */
    g_snd_samples = calloc(1, 0x800000);
    for (int i = 0; i < 0x200000; i++) { g_snd_samples[i * 2] = roms[12].data[i]; g_snd_samples[0x400000 + i * 2] = roms[13].data[i]; }
    for (int i = 0; i < NROMS; i++) { free(roms[i].data); roms[i].data = NULL; }
    core_eeprom_factory();
    snd_init(g_snd_prog, 0x80000, g_snd_samples, 0x800000);
    video_init();
    cpu_init();
    return 1;
}

void core_reset(void) {
    memset(g_ram, 0, sizeof g_ram);
    memset(g_palram, 0, sizeof g_palram);
    memset(g_spriteram, 0, sizeof g_spriteram);
    memset(g_pfram, 0, sizeof g_pfram);
    memset(g_textram, 0, sizeof g_textram);
    memset(g_charram, 0, sizeof g_charram);
    memset(g_lineram, 0, sizeof g_lineram);
    memset(g_pivotram, 0, sizeof g_pivotram);
    memset(g_ctrl0, 0, sizeof g_ctrl0); memset(g_ctrl1, 0, sizeof g_ctrl1);
    memset(g_dpram, 0, sizeof g_dpram);
    coin_word[0] = coin_word[1] = 0; timer_ctrl = 0;
    g_cycles = 0; g_frame_count = 0;
    eeprom_reset();
    video_reset();
    cpu_reset();
    snd_reset();       /* sound CPU starts held in reset (MAME machine_reset) */
}

void core_run_frame(cpu_mode_t mode) {
    g_frame_done = 0; irq3_done = 0;
    ev_snd = (g_cycles / SND_SLICE + 1) * SND_SLICE;
    cpu_set_irq_line(2);                  /* vblank */
    compute_next_event();
    core_service();
    if (!g_frame_done) { if (mode == CPU_MODE_RECOMP) rc_run(); else cpu_interp_run(); }
    snd_run_until(CYCLES_PER_FRAME);
    snd_frame_end();
    g_cycles -= CYCLES_PER_FRAME;
    video_frame(fb);
    g_frame_count++;
}

const uint32_t *core_framebuffer(void) { return fb; }

uint32_t cpu_state_hash_regs(uint32_t h); /* cpu.c */
static uint32_t hash_buf(uint32_t h, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
uint32_t core_state_hash(void) {
    uint32_t h = 2166136261u;
    h = hash_buf(h, g_ram, sizeof g_ram);
    h = hash_buf(h, g_spriteram, sizeof g_spriteram);
    h = hash_buf(h, g_palram, sizeof g_palram);
    h = hash_buf(h, g_pfram, sizeof g_pfram);
    h = hash_buf(h, g_lineram, sizeof g_lineram);
    h = hash_buf(h, g_textram, sizeof g_textram);
    h = hash_buf(h, g_dpram, sizeof g_dpram);
    h = (h ^ (uint32_t)g_cycles) * 16777619u;
    h = snd_state_hash(h);
    return cpu_state_hash_regs(h);
}

/* ------------------------------------------------------------------ save states */
void hw_state(sstream_t *s) {
    ss_io(s, g_ram, sizeof g_ram); ss_io(s, g_palram, sizeof g_palram);
    ss_io(s, g_spriteram, sizeof g_spriteram); ss_io(s, g_pfram, sizeof g_pfram);
    ss_io(s, g_textram, sizeof g_textram); ss_io(s, g_charram, sizeof g_charram);
    ss_io(s, g_lineram, sizeof g_lineram); ss_io(s, g_pivotram, sizeof g_pivotram);
    ss_io(s, g_ctrl0, sizeof g_ctrl0); ss_io(s, g_ctrl1, sizeof g_ctrl1);
    ss_io(s, g_dpram, sizeof g_dpram);
    ss_io(s, g_eeprom, sizeof g_eeprom);
    SS(s, g_cycles); SS(s, g_next_event); SS(s, g_frame_count);
    SS(s, irq3_done); SS(s, ev_snd); SS(s, coin_word); SS(s, timer_ctrl);
    SS(s, ee_state); SS(s, ee_cs); SS(s, ee_clk); SS(s, ee_di); SS(s, ee_bits); SS(s, ee_accum);
    SS(s, ee_locked); SS(s, ee_cmd); SS(s, ee_addr); SS(s, ee_shift);
}

/* state blob: magic, format version, ROM fingerprint, then the devices */
#define STATE_MAGIC   0x474c4b31u /* "GLK1" */
#define STATE_VERSION 1u
static void all_state(sstream_t *s) { hw_state(s); snd_state(s); video_state(s); cpu_state(s); }
size_t core_state_save(uint8_t **out) {
    sstream_t s = {0};
    uint32_t magic = STATE_MAGIC, ver = STATE_VERSION, fp = rom_fp;
    SS(&s, magic); SS(&s, ver); SS(&s, fp);
    all_state(&s);
    uint32_t end = 0x454e4421u; SS(&s, end);
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, s.p, s.pos); SS(&s, crc);   /* integrity check */
    if (s.error) { free(s.p); *out = NULL; return 0; }
    *out = s.p; return s.pos;
}
int core_state_load(const uint8_t *data, size_t n) {
    sstream_t s = {(uint8_t *)data, 0, n, 1, 0};
    uint32_t magic = 0, ver = 0, fp = 0;
    SS(&s, magic); SS(&s, ver); SS(&s, fp);
    if (s.error || magic != STATE_MAGIC || ver != STATE_VERSION || fp != rom_fp) return 0;
    if (n < 16) return 0;
    uint32_t crc; memcpy(&crc, data + n - 4, 4);
    if ((uint32_t)mz_crc32(MZ_CRC32_INIT, data, n - 4) != crc) return 0;          /* damaged */
    s.cap = n - 4;
    /* full dry run on a backup so that a damaged blob never half-applies */
    uint8_t *backup = NULL; size_t bn = core_state_save(&backup);
    if (!bn) return 0;
    all_state(&s);
    uint32_t end = 0; SS(&s, end);
    if (s.error || s.pos != n - 4 || end != 0x454e4421u) {
        sstream_t b = {backup, 12, bn, 1, 0}; all_state(&b);
        free(backup); return 0;
    }
    free(backup);
    return 1;
}
