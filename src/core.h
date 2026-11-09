/* Gunlock / RayForce static recompilation - core hardware interface
 * Taito F3 system (68EC020 main CPU, TC0630FDP video, Taito EN sound: 68000 + ES5505)
 * Hardware behaviour as documented by MAME's taito/taito_f3*.cpp, taito_en.cpp,
 * sound/es5506.cpp, machine/mc68681.cpp (BSD-3-Clause; Bryan McPhail, ywy, 12Me21,
 * Aaron Giles, R. Belmont, Philip Bennett and others).
 */
#ifndef GL_CORE_H
#define GL_CORE_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>

#define CPU_CLOCK        16000000
/* screen: 26.686MHz/4 pixel clock, 432 x 262 total -> 58.94 Hz */
#define PIXEL_CLOCK_X4   26686000
#define H_TOTAL 432
#define V_TOTAL 262
#define CYCLES_PER_LINE_X1000 1036049     /* 16e6*432*4/26.686e6 * 1000 */
#define CYCLES_PER_FRAME 271445          /* 16e6 * 432*262*4 / 26.686e6 (58.94 Hz) */
#define IRQ3_DELAY       10000           /* MAME: int3 10000 cycles after vblank */
#define SND_CLOCK        15238090        /* 30.47618MHz / 2 */
#define SND_CYCLES_PER_FRAME 258519      /* SND_CLOCK / frame rate */

#define SCREEN_W 320
#define SCREEN_H 224                     /* f3_224a visible area: y 31..254 */
#define SCREEN_X0 46
#define SCREEN_Y0 31
#define AUDIO_RATE 44100

/* ---------- main memory (big-endian byte arrays) ---------- */
extern uint8_t g_rom[0x200000];          /* 0x000000 program ROM (1MB populated) */
extern uint8_t g_ram[0x20000];           /* 0x400000 work RAM, mirrored at 0x420000 */
extern uint8_t g_palram[0x8000];         /* 0x440000 */
extern uint8_t g_spriteram[0x10000];     /* 0x600000 */
extern uint8_t g_pfram[0xc000];          /* 0x610000 */
extern uint8_t g_textram[0x2000];        /* 0x61c000 */
extern uint8_t g_charram[0x2000];        /* 0x61e000 */
extern uint8_t g_lineram[0x10000];       /* 0x620000 */
extern uint8_t g_pivotram[0x10000];      /* 0x630000 */
extern uint16_t g_ctrl0[8], g_ctrl1[8];  /* 0x660000 / 0x660010 */
extern uint8_t g_dpram[0x800];           /* 0xc00000 shared with the sound CPU */

uint32_t mem_read8(uint32_t a);
uint32_t mem_read16(uint32_t a);
uint32_t mem_read32(uint32_t a);
void     mem_write8(uint32_t a, uint32_t v);
void     mem_write16(uint32_t a, uint32_t v);
void     mem_write32(uint32_t a, uint32_t v);

/* fast inline paths for the recompiled code. The 68EC020 allows misaligned
 * word/long accesses, so byte-wise assembly is used (no alignment masking). */
#define RAM_HIT(a, n) (((a) & 0xfc0000) == 0x400000 && ((a) & 0x1ffff) <= 0x20000 - (n))
static inline uint32_t rd8(uint32_t a) {
    a &= 0xffffff;
    if ((a & 0xfc0000) == 0x400000) return g_ram[a & 0x1ffff];
    if (a < 0x200000) return g_rom[a];
    return mem_read8(a);
}
static inline uint32_t rd16(uint32_t a) {
    a &= 0xffffff;
    if (RAM_HIT(a, 2)) { const uint8_t *p = &g_ram[a & 0x1ffff]; return (p[0] << 8) | p[1]; }
    if (a < 0x1fffff) { const uint8_t *p = &g_rom[a]; return (p[0] << 8) | p[1]; }
    return mem_read16(a);
}
static inline uint32_t rd32(uint32_t a) {
    a &= 0xffffff;
    if (RAM_HIT(a, 4)) { const uint8_t *p = &g_ram[a & 0x1ffff];
        return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
    if (a < 0x1ffffd) { const uint8_t *p = &g_rom[a];
        return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
    return mem_read32(a);
}
static inline void wr8(uint32_t a, uint32_t v) {
    a &= 0xffffff;
    if ((a & 0xfc0000) == 0x400000) { g_ram[a & 0x1ffff] = (uint8_t)v; return; }
    mem_write8(a, v);
}
static inline void wr16(uint32_t a, uint32_t v) {
    a &= 0xffffff;
    if (RAM_HIT(a, 2)) { uint8_t *p = &g_ram[a & 0x1ffff]; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
    mem_write16(a, v);
}
static inline void wr32(uint32_t a, uint32_t v) {
    a &= 0xffffff;
    if (RAM_HIT(a, 4)) { uint8_t *p = &g_ram[a & 0x1ffff];
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return; }
    mem_write32(a, v);
}
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* ---------- timing / irq ---------- */
extern int64_t  g_cycles;        /* main CPU cycles elapsed in current frame */
extern int64_t  g_next_event;    /* next cycle count at which core_service() must run */
extern int      g_frame_done;
extern uint64_t g_frame_count;
void core_service(void);
void core_irq_changed(void);

/* ---------- inputs ---------- */
enum {
    IN_UP = 0x01, IN_DOWN = 0x02, IN_LEFT = 0x04, IN_RIGHT = 0x08,
    IN_B1 = 0x10, IN_B2 = 0x20, IN_B3 = 0x40, IN_START = 0x80,
    IN_COIN = 0x100, IN_SERVICE = 0x200, IN_TEST = 0x400, IN_B4 = 0x800, IN_TILT = 0x1000
};
extern uint16_t g_input[2];      /* active-high bitmask per player */

/* ---------- lifecycle ---------- */
typedef enum { CPU_MODE_RECOMP = 0, CPU_MODE_INTERP = 1 } cpu_mode_t;
int  core_load_romset(const char *path);           /* zip or directory */
void core_reset(void);
void core_run_frame(cpu_mode_t mode);
const uint32_t *core_framebuffer(void);            /* SCREEN_W*SCREEN_H XRGB8888 (unrotated) */
int  core_audio_samples(int16_t *out, int max_frames); /* stereo interleaved, drains buffer */
const char *core_last_error(void);
const char *core_romset_name(void);
uint32_t core_rom_fingerprint(void);
void core_eeprom_load(const char *path);
void core_eeprom_save(const char *path);
void core_eeprom_factory(void);
uint32_t core_state_hash(void);
size_t core_state_save(uint8_t **out);              /* malloc'd blob, call between frames only */
int  core_state_load(const uint8_t *data, size_t n); /* 1 = ok; atomic (restores on failure) */
extern const uint32_t rc_stats_native, rc_stats_fallback;

/* subsystem hooks */
void video_init(void);
void video_reset(void);
void video_frame(uint32_t *fb);         /* MAME screen_update order for this game (sprite lag 2) */
void eeprom_reset(void);
void eeprom_write_lines(int di, int cs, int clk);
int  eeprom_do(void);
extern uint16_t g_eeprom[64];

/* sound board (sound.c) */
void snd_init(const uint8_t *prog, uint32_t prog_size, const uint8_t *samples, uint32_t sample_size);
void snd_reset(void);
void snd_set_reset_line(int asserted);
void snd_run_until(int64_t main_cycle);  /* catch the sound CPU up to a main-CPU time point */
void snd_frame_end(void);
uint32_t snd_state_hash(uint32_t h);

/* graphics ROM data (decoded 8bpp, 16x16 tiles, 256 bytes each) */
extern uint8_t *g_spr_gfx;   extern uint32_t g_spr_count;
extern uint8_t *g_tile_gfx;  extern uint32_t g_tile_count;

/* recompiled program entry (generated) */
void rc_run(void);
int  rc_has(uint32_t pc);
/* cpu glue */
void cpu_init(void);
void cpu_reset(void);
void cpu_interp_run(void);
void cpu_set_irq_line(int level);
void cpu_coverage_enable(const char *path);
void cpu_coverage_flush(void);
extern int g_trace_unknown;
#ifdef __cplusplus
}
#endif
#endif
