/* Gunlock recomp - Taito EN sound board
 *   68000 @ 15.238 MHz (Musashi context, see cpu.c), 64KB RAM, MB8421 dual-port RAM shared with the main CPU
 *   Ensoniq ES5505 "OTIS" 32-voice sample player (C port of MAME's sound/es5506.cpp, ES5505 path)
 *   Motorola MC68681 DUART: only its counter/timer and interrupt logic are used (sound driver tick, IRQ6)
 *   Ensoniq ES5510 "ESP" effects DSP: host interface registers only; audio passes through dry
 *   Fujitsu MB87078 electronic volume
 * Reference: MAME taito/taito_en.cpp, sound/es5506.cpp, machine/mc68681.cpp, cpu/es5510/es5510.cpp,
 * sound/esqpump.cpp (BSD-3-Clause).
 */
#include "core.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "state.h"
#include "m68k.h"
#include <stdio.h>

int  cpu_snd_execute(int cycles);
void cpu_snd_set_irq(int level);
void cpu_snd_reset(void);

static const uint8_t *prog;           /* 512KB at 0xc00000 */
static const uint8_t *srom;           /* ES5505 sample ROM, 16-bit BE words */
static uint32_t srom_words_mask;

static uint8_t sram[0x10000];

/* ------------------------------------------------------------------ timing */
static int64_t snd_now;               /* absolute sound-CPU cycle count */
static int64_t frame_base;            /* sound cycle at the start of the current main frame */
static int     in_reset;
static int     slice_active;          /* inside m68k_execute */
static int64_t slice_start;

static int64_t cur_time(void) {
    if (slice_active) return slice_start + m68k_cycles_run();
    return snd_now;
}

/* ------------------------------------------------------------------ ES5505 */
#define FILTER_SHIFT 4
#define FILTER_BIT 12
#define ADDRESS_FRAC_BIT 11
#define ACC_SHIFT 2                 /* ADDRESS_FRAC_BIT - 9 */
#define ACC_MASK 0x7fffffffull
#define VOLUME_ACC_SHIFT 11

enum { C_IRQ = 0x80, C_DIR = 0x40, C_IRQE = 0x20, C_BLE = 0x10, C_LPE = 0x08, C_STOP1 = 2, C_STOP0 = 1,
       C_LOOPMASK = C_BLE | C_LPE, C_STOPMASK = C_STOP1 | C_STOP0 };

typedef struct {
    uint32_t control;
    uint64_t freqcount, start, end, accum;
    uint32_t lvol, rvol, k1, k2;
    int32_t o4n1, o3n1, o3n2, o2n1, o2n2, o1n1;
} esvoice_t;

static esvoice_t ev[32];
static uint8_t es_page, es_active = 0x1f, es_irqv = 0x80;
static uint16_t es_mode;
static uint16_t otisbank[32];
static uint32_t vol_lookup[256];
static int64_t es_next;               /* sound cycle at which the next ES5505 sample is due */

static inline uint16_t read_sample(int v, uint32_t addr) {
    uint32_t w = ((uint32_t)(otisbank[v] & 3) << 20) + addr;
    w &= srom_words_mask;
    return (uint16_t)((srom[w * 2] << 8) | srom[w * 2 + 1]);
}
static inline uint32_t int_addr(uint64_t accum, int bias) {
    return (uint32_t)(((accum + ((uint64_t)bias << ADDRESS_FRAC_BIT)) & ACC_MASK) >> ADDRESS_FRAC_BIT);
}
static inline int32_t interp(int32_t s1, int32_t s2, uint64_t accum) {
    uint32_t a = (uint32_t)(accum & ((1u << ADDRESS_FRAC_BIT) - 1));
    return (s1 * (int32_t)((1 << ADDRESS_FRAC_BIT) - a) + s2 * (int32_t)a) >> ADDRESS_FRAC_BIT;
}
static inline int32_t lowpass(int32_t out, int32_t cutoff, int32_t in) {
    return ((int32_t)(cutoff >> FILTER_SHIFT) * (out - in) / (1 << FILTER_BIT)) + in;
}
static inline int32_t highpass(int32_t out, int32_t cutoff, int32_t in, int32_t prev) {
    return out - prev + ((int32_t)(cutoff >> FILTER_SHIFT) * in) / (1 << (FILTER_BIT + 1)) + in / 2;
}
static void apply_filters(esvoice_t *v, int32_t *s) {
    int32_t x = *s;
    x = lowpass(x, v->k1, v->o1n1); v->o1n1 = x;
    x = lowpass(x, v->k1, v->o2n1); v->o2n2 = v->o2n1; v->o2n1 = x;
    switch ((v->control >> 10) & 3) {
    case 0:
        x = highpass(x, v->k2, v->o3n1, v->o2n2); v->o3n2 = v->o3n1; v->o3n1 = x;
        x = highpass(x, v->k2, v->o4n1, v->o3n2); v->o4n1 = x; break;
    case 1:
        x = lowpass(x, v->k1, v->o3n1); v->o3n2 = v->o3n1; v->o3n1 = x;
        x = highpass(x, v->k2, v->o4n1, v->o3n2); v->o4n1 = x; break;
    case 2:
        x = lowpass(x, v->k2, v->o3n1); v->o3n2 = v->o3n1; v->o3n1 = x;
        x = lowpass(x, v->k2, v->o4n1); v->o4n1 = x; break;
    case 3:
        x = lowpass(x, v->k1, v->o3n1); v->o3n2 = v->o3n1; v->o3n1 = x;
        x = lowpass(x, v->k2, v->o4n1); v->o4n1 = x; break;
    }
    *s = x;
}
static inline int64_t get_sample(int32_t s, uint32_t vol) { return ((int64_t)s * vol_lookup[vol & 0xff]) >> VOLUME_ACC_SHIFT; }

static void voice_step(int vn, int32_t *dest) {
    esvoice_t *v = &ev[vn];
    uint64_t accum = v->accum & ACC_MASK;
    if (!(v->control & C_STOPMASK)) {
        int32_t s1 = (int16_t)read_sample(vn, int_addr(accum, 0));
        int32_t s2 = (int16_t)read_sample(vn, int_addr(accum, 1));
        s1 = interp(s1, s2, accum);
        if (!(v->control & C_DIR)) accum = (accum + v->freqcount) & ACC_MASK;
        else accum = (accum - v->freqcount) & ACC_MASK;
        apply_filters(v, &s1);
        dest[0] += (int32_t)get_sample(s1, v->lvol);
        dest[1] += (int32_t)get_sample(s1, v->rvol);
        if (!(v->control & C_DIR)) {
            if (accum > v->end) {
                if (v->control & C_IRQE) v->control |= C_IRQ;
                switch (v->control & C_LOOPMASK) {
                case 0: case C_BLE: v->control |= C_STOP0; break;
                case C_LPE: accum = (v->start + (accum - v->end)) & ACC_MASK; break;
                default: accum = (v->end - (accum - v->end)) & ACC_MASK; v->control ^= C_DIR; break;
                }
            }
        } else {
            if (accum < v->start) {
                if (v->control & C_IRQE) v->control |= C_IRQ;
                switch (v->control & C_LOOPMASK) {
                case 0: case C_BLE: v->control |= C_STOP0; break;
                case C_LPE: accum = (v->end - (v->start - accum)) & ACC_MASK; break;
                default: accum = (v->start + (v->start - accum)) & ACC_MASK; v->control ^= C_DIR; break;
                }
            }
        }
    }
    v->accum = accum;
    if (v->control & C_IRQ) {
        if (es_irqv & 0x80) { es_irqv = vn & 0x1f; v->control &= ~C_IRQ; }
    }
}

/* ------------------------------------------------------------------ output mixing / resampling */
static double mb_gain[4] = {1, 1, 1, 1};   /* MB87078 channel gains (percent/32, as MAME) */
static uint8_t mb_latch0[4], mb_latch1[4], mb_ch;

#define OUTBUF (AUDIO_RATE)
static int16_t outbuf[OUTBUF * 2];
static int out_w, out_r, out_count;
static double rs_pos;                      /* resampler phase in ES samples */
static float rs_prevL, rs_prevR, rs_curL, rs_curR;

static void push_out(float l, float r) {
    int32_t il = (int32_t)lrintf(l * 32767.0f), ir = (int32_t)lrintf(r * 32767.0f);
    if (il > 32767) il = 32767; if (il < -32768) il = -32768;
    if (ir > 32767) ir = 32767; if (ir < -32768) ir = -32768;
    if (out_count >= OUTBUF) { out_r = (out_r + 1) % OUTBUF; out_count--; }
    outbuf[out_w * 2] = (int16_t)il; outbuf[out_w * 2 + 1] = (int16_t)ir;
    out_w = (out_w + 1) % OUTBUF; out_count++;
}

static void es_generate_one(void) {
    int32_t cur[8] = {0};
    for (int v = 0; v <= es_active; v++) {
        int ch = (ev[v].control >> 8) & 3;     /* 4 output channels configured */
        voice_step(v, &cur[ch * 2]);
    }
    float o[8];
    for (int c = 0; c < 8; c++) {
        int32_t x = cur[c];
        if (x > (1 << 19)) x = 1 << 19; if (x < -(1 << 19)) x = -(1 << 19);
        /* ES5505 output -> pump (0.18) with MB87078 gain on left (ch2) / right (ch3) outputs */
        o[c] = (float)x / (float)(1 << 19) * 0.18f * (float)mb_gain[2 + (c & 1)];
    }
    /* esqpump: channel 0 is the 'aux' pair (direct), channels 1-3 feed the ES5510.
     * The ESP program is not executed: its inputs are summed dry. Pump outputs: x0.5 and MB87078 gain. */
    float L = (o[0] + o[2] + o[4] + o[6]) * 0.5f * (float)mb_gain[2];
    float R = (o[1] + o[3] + o[5] + o[7]) * 0.5f * (float)mb_gain[3];
    /* linear resampling to AUDIO_RATE */
    rs_prevL = rs_curL; rs_prevR = rs_curR; rs_curL = L; rs_curR = R;
    double es_rate = (double)SND_CLOCK / (16.0 * (es_active + 1));
    double step = es_rate / AUDIO_RATE;
    while (rs_pos < 1.0) {
        float t = (float)rs_pos;
        push_out(rs_prevL + (rs_curL - rs_prevL) * t, rs_prevR + (rs_curR - rs_prevR) * t);
        rs_pos += step;
    }
    rs_pos -= 1.0;
}

static void es_sync(int64_t t) {
    while (es_next <= t) {
        es_generate_one();
        es_next += 16 * (es_active + 1);
    }
}

/* ES5505 register interface */
static uint64_t acc_shift(uint64_t v, int bias) { return (ACC_SHIFT - bias >= 0) ? v << (ACC_SHIFT - bias) : v >> (bias - ACC_SHIFT); }
static uint64_t acc_res(uint64_t v, int bias) { return (ACC_SHIFT - bias >= 0) ? v >> (ACC_SHIFT - bias) : v << (bias - ACC_SHIFT); }
#define LO8 (mask & 0x00ff)
#define HI8 (mask & 0xff00)
static void es_write(int off, uint16_t data, uint16_t mask) {
    es_sync(cur_time());
    esvoice_t *v = &ev[es_page & 0x1f];
    if (off == 0x0d) { if (LO8) es_active = data & 0x1f; return; }
    if (off == 0x0f) { if (LO8) es_page = data & 0x7f; return; }
    if (off == 0x0e) return;
    if (es_page < 0x20) {
        switch (off) {
        case 0x00: v->control |= 0xf000; if (LO8) { v->control &= ~0x00ff; v->control |= data & 0x00ff; }
                   if (HI8) v->control = (v->control & ~0x0f00) | (data & 0x0f00); break;
        case 0x01: if (LO8) v->freqcount = (v->freqcount & ~acc_shift(0x00fe, 1)) | acc_shift(data & 0x00fe, 1);
                   if (HI8) v->freqcount = (v->freqcount & ~acc_shift(0xff00, 1)) | acc_shift(data & 0xff00, 1); break;
        case 0x02: if (LO8) v->start = (v->start & ~acc_shift(0x00ff0000, 0)) | acc_shift((uint64_t)(data & 0x00ff) << 16, 0);
                   if (HI8) v->start = (v->start & ~acc_shift(0x1f000000, 0)) | acc_shift((uint64_t)(data & 0x1f00) << 16, 0); break;
        case 0x03: if (LO8) v->start = (v->start & ~acc_shift(0xe0, 0)) | acc_shift(data & 0x00e0, 0);
                   if (HI8) v->start = (v->start & ~acc_shift(0xff00, 0)) | acc_shift(data & 0xff00, 0); break;
        case 0x04: if (LO8) v->end = (v->end & ~acc_shift(0x00ff0000, 0)) | acc_shift((uint64_t)(data & 0x00ff) << 16, 0);
                   if (HI8) v->end = (v->end & ~acc_shift(0x1f000000, 0)) | acc_shift((uint64_t)(data & 0x1f00) << 16, 0); break;
        case 0x05: if (LO8) v->end = (v->end & ~acc_shift(0xe0, 0)) | acc_shift(data & 0x00e0, 0);
                   if (HI8) v->end = (v->end & ~acc_shift(0xff00, 0)) | acc_shift(data & 0xff00, 0); break;
        case 0x06: if (LO8) v->k2 = (v->k2 & ~0x00f0) | (data & 0x00f0); if (HI8) v->k2 = (v->k2 & ~0xff00) | (data & 0xff00); break;
        case 0x07: if (LO8) v->k1 = (v->k1 & ~0x00f0) | (data & 0x00f0); if (HI8) v->k1 = (v->k1 & ~0xff00) | (data & 0xff00); break;
        case 0x08: if (HI8) v->lvol = (v->lvol & ~0xff) | ((data & 0xff00) >> 8); break;
        case 0x09: if (HI8) v->rvol = (v->rvol & ~0xff) | ((data & 0xff00) >> 8); break;
        case 0x0a: if (LO8) v->accum = (v->accum & ~acc_shift(0x00ff0000, 0)) | acc_shift((uint64_t)(data & 0x00ff) << 16, 0);
                   if (HI8) v->accum = (v->accum & ~acc_shift(0x1f000000, 0)) | acc_shift((uint64_t)(data & 0x1f00) << 16, 0); break;
        case 0x0b: if (LO8) v->accum = (v->accum & ~acc_shift(0xff, 0)) | acc_shift(data & 0x00ff, 0);
                   if (HI8) v->accum = (v->accum & ~acc_shift(0xff00, 0)) | acc_shift(data & 0xff00, 0); break;
        }
    } else if (es_page < 0x40) {
        int32_t *r = NULL;
        switch (off) {
        case 0x00: v->control |= 0xf000; if (LO8) v->control = (v->control & ~0x00ff) | (data & 0x00ff);
                   if (HI8) v->control = (v->control & ~0x0f00) | (data & 0x0f00); return;
        case 0x01: r = &v->o4n1; break; case 0x02: r = &v->o3n1; break; case 0x03: r = &v->o3n2; break;
        case 0x04: r = &v->o2n1; break; case 0x05: r = &v->o2n2; break; case 0x06: r = &v->o1n1; break;
        }
        if (r) {
            if (LO8) *r = (*r & ~0x00ff) | (data & 0x00ff);
            if (HI8) *r = (int16_t)((*r & ~0xff00) | (data & 0xff00));
        }
    } else {
        if (off == 0x08) { es_mode |= 0x7f8; if (HI8) es_mode = (es_mode & ~0xf800) | (data & 0xf800); if (LO8) es_mode = (es_mode & ~7) | (data & 7); }
    }
}
static uint16_t es_read(int off) {
    es_sync(cur_time());
    esvoice_t *v = &ev[es_page & 0x1f];
    if (off == 0x0d) return es_active;
    if (off == 0x0f) return es_page;
    if (off == 0x0e) { uint16_t r = es_irqv; es_irqv = 0x80; return r; }
    if (es_page < 0x20) {
        switch (off) {
        case 0x00: return (uint16_t)(v->control | 0xf000);
        case 0x01: return (uint16_t)acc_res(v->freqcount, 1);
        case 0x02: return (uint16_t)(acc_res(v->start, 0) >> 16);
        case 0x03: return (uint16_t)acc_res(v->start, 0);
        case 0x04: return (uint16_t)(acc_res(v->end, 0) >> 16);
        case 0x05: return (uint16_t)acc_res(v->end, 0);
        case 0x06: return (uint16_t)v->k2;
        case 0x07: return (uint16_t)v->k1;
        case 0x08: return (uint16_t)(v->lvol << 8);
        case 0x09: return (uint16_t)(v->rvol << 8);
        case 0x0a: return (uint16_t)(acc_res(v->accum, 0) >> 16);
        case 0x0b: return (uint16_t)acc_res(v->accum, 0);
        }
        return 0;
    } else if (es_page < 0x40) {
        switch (off) {
        case 0x00: return (uint16_t)(v->control | 0xf000);
        case 0x01: return (uint16_t)v->o4n1; case 0x02: return (uint16_t)v->o3n1; case 0x03: return (uint16_t)v->o3n2;
        case 0x04: return (uint16_t)v->o2n1; case 0x05: return (uint16_t)v->o2n2;
        case 0x06:
            /* F3 games read raw sample ROM data through O1(n-1) of a stopped voice (as MAME) */
            if (v->control & C_STOPMASK) v->o1n1 = read_sample((int)(v - ev), int_addr(v->accum, 0));
            return (uint16_t)v->o1n1;
        }
        return 0;
    } else {
        if (off == 0x08) return es_mode | 0x7f8;
        return 0;
    }
}

/* ------------------------------------------------------------------ MB87078 */
static void mb_update(int ch) {
    int idx;
    if (!(mb_latch1[ch] & 0x04)) idx = 65;
    else if (mb_latch1[ch] & 0x10) idx = 0;
    else if (mb_latch1[ch] & 0x08) idx = 64;
    else idx = (mb_latch0[ch] ^ 0x3f) & 0x3f;
    int pct = idx >= 65 ? 0 : (int)(100.0 * pow(10.0, -0.5 * idx / 20.0) + 0.5);
    mb_gain[ch] = pct / 32.0;
}
static void mb_write(int off, uint8_t d) {
    es_sync(cur_time());
    if (off == 0) mb_latch0[mb_ch] = d & 0x3f;
    else { mb_ch = d & 3; mb_latch1[mb_ch] = d & 0x1f; }
    mb_update(mb_ch);
}

/* ------------------------------------------------------------------ MC68681 DUART (timer + irq) */
static uint8_t du_acr, du_imr, du_isr, du_ivr = 0x0f, du_ctur, du_ctlr, du_opcr, du_half;
static int du_running;
static int64_t du_next;               /* sound cycle of next timer expiry */
#define DUART_CLOCK 4000000           /* 16 MHz / 4 */

static uint32_t du_rate(void) {
    switch ((du_acr >> 4) & 3) { case 3: return DUART_CLOCK / 16; default: return DUART_CLOCK; }
}
static void du_irq_update(void) { cpu_snd_set_irq((du_isr & du_imr) ? 6 : 0); }
static void du_start(int64_t now, uint32_t count) {
    uint32_t r = du_rate();
    du_next = now + ((int64_t)count * SND_CLOCK + r / 2) / r;
    if (du_next <= now) du_next = now + 1;
    du_running = 1;
}
static uint16_t du_count(void) { uint16_t c = (uint16_t)((du_ctur << 8) | du_ctlr); return c ? c : 1; }
static void du_fire(void) {
    int64_t t = du_next;
    if (du_acr & 0x40) {
        du_half ^= 1;
        if (!du_half) { du_isr |= 0x08; du_irq_update(); }
        du_start(t, du_count());
    } else {
        du_isr |= 0x08; du_irq_update();
        du_start(t, 0xffff);
    }
}
static uint8_t du_read(int off) {
    switch (off) {
    case 0x01: case 0x09: return 0x0c;            /* SRA/SRB: TxRDY | TxEMT */
    case 0x04: return 0;                          /* IPCR */
    case 0x05: return du_isr;
    case 0x06: case 0x07: {
        int64_t now = cur_time();
        uint32_t rem = du_running && du_next > now ? (uint32_t)((du_next - now) * (int64_t)du_rate() / SND_CLOCK) : 0;
        return off == 6 ? (uint8_t)(rem >> 8) : (uint8_t)rem;
    }
    case 0x0a: return 0x61;
    case 0x0c: return du_ivr;
    case 0x0d: return 0x80 | 0x03;                /* IP: bit 7 always set; IP0/IP1 tied to 5V */
    case 0x0e:                                    /* start counter command */
        if (du_acr & 0x40) du_half = 0;
        du_start(cur_time(), du_count());
        return 0xff;
    case 0x0f:                                    /* stop counter command */
        if (!(du_acr & 0x40)) du_running = 0;
        du_isr &= ~0x08; du_irq_update();
        return 0xff;
    }
    return 0xff;
}
static void du_write(int off, uint8_t d) {
    switch (off) {
    case 0x04: {
        uint8_t old = du_acr; du_acr = d;
        if ((old ^ d) & 0x40) {
            if (d & 0x40) { du_half = 0; du_start(cur_time(), du_count()); }
            else du_running = 0;
        }
        break;
    }
    case 0x05: du_imr = d; du_irq_update(); break;
    case 0x06: du_ctur = d; break;
    case 0x07: du_ctlr = d; break;
    case 0x0c: du_ivr = d; break;
    case 0x0d: du_opcr = d; break;
    default: break;                               /* serial channels / output port (ESPHALT): ignored */
    }
}
int snd_int_ack(int level) {
    if (level == 6) return du_ivr;
    return M68K_INT_ACK_AUTOVECTOR;
}

/* ------------------------------------------------------------------ ES5510 host interface (DSP not executed) */
static uint32_t esp_gpr[0xc0], esp_gpr_latch, esp_dil, esp_dol, esp_dadr;
static uint64_t esp_instr[0xa0], esp_instr_latch;
static uint8_t esp_host_ctrl, esp_ram_sel, esp_serial;
static uint8_t esp_read(int off) {
    switch (off) {
    case 0x00: return (esp_gpr_latch >> 16) & 0xff;
    case 0x01: return (esp_gpr_latch >> 8) & 0xff;
    case 0x02: return esp_gpr_latch & 0xff;
    case 0x03: return (esp_instr_latch >> 40) & 0xff; case 0x04: return (esp_instr_latch >> 32) & 0xff;
    case 0x05: return (esp_instr_latch >> 24) & 0xff; case 0x06: return (esp_instr_latch >> 16) & 0xff;
    case 0x07: return (esp_instr_latch >> 8) & 0xff;  case 0x08: return esp_instr_latch & 0xff;
    case 0x09: return (esp_dil >> 16) & 0xff; case 0x0a: return (esp_dil >> 8) & 0xff; case 0x0b: return 0;
    case 0x0c: return (esp_dol >> 16) & 0xff; case 0x0d: return (esp_dol >> 8) & 0xff; case 0x0e: return 0xff;
    case 0x0f: return (esp_dadr >> 16) & 0xff; case 0x10: return (esp_dadr >> 8) & 0xff; case 0x11: return esp_dadr & 0xff;
    case 0x12: return 0;
    case 0x16: return 0x27;
    }
    return 0;
}
static void esp_write(int off, uint8_t d) {
    switch (off) {
    case 0x00: esp_gpr_latch = (esp_gpr_latch & 0x00ffff) | (d << 16); break;
    case 0x01: esp_gpr_latch = (esp_gpr_latch & 0xff00ff) | (d << 8); break;
    case 0x02: esp_gpr_latch = (esp_gpr_latch & 0xffff00) | d; break;
    case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: {
        int sh = (8 - off) * 8;
        esp_instr_latch = (esp_instr_latch & ~(0xffull << sh)) | ((uint64_t)d << sh); break;
    }
    case 0x0c: esp_dol = (esp_dol & 0x00ffff) | (d << 16); break;
    case 0x0d: esp_dol = (esp_dol & 0xff00ff) | (d << 8); break;
    case 0x0e: esp_dol = (esp_dol & 0xffff00) | d; break;
    case 0x0f: esp_dadr = (esp_dadr & 0x00ffff) | (d << 16); if (esp_ram_sel) esp_dil = 0; break;
    case 0x10: esp_dadr = (esp_dadr & 0xff00ff) | (d << 8); break;
    case 0x11: esp_dadr = (esp_dadr & 0xffff00) | d; break;
    case 0x12: esp_host_ctrl = (esp_host_ctrl & 4) | (d & 3); esp_host_ctrl &= ~2; break;
    case 0x14: esp_ram_sel = d & 0x80; break;
    case 0x18: esp_serial = d; break;
    case 0x80: if (d < 0xa0) esp_instr_latch = esp_instr[d]; if (d < 0xc0) esp_gpr_latch = esp_gpr[d] & 0xffffff; break;
    case 0xa0: if (d < 0xc0) esp_gpr[d] = esp_gpr_latch; break;
    case 0xc0: if (d < 0xa0) esp_instr[d] = esp_instr_latch & 0xffffffffffffull; break;
    case 0xe0: if (d < 0xa0) esp_instr[d] = esp_instr_latch & 0xffffffffffffull; if (d < 0xc0) esp_gpr[d] = esp_gpr_latch; break;
    }
}

/* ------------------------------------------------------------------ 68000 bus */
uint32_t snd_read16(uint32_t a) {
    a &= 0xfffffe;
    if (a < 0x40000) return (sram[a & 0xffff] << 8) | sram[(a & 0xffff) + 1];
    if (a >= 0xc00000 && a < 0xc80000) return (prog[a - 0xc00000] << 8) | prog[a - 0xc00000 + 1];
    if (a >= 0xff0000) return (sram[a & 0xffff] << 8) | sram[(a & 0xffff) + 1];
    if (a >= 0x140000 && a < 0x141000) return (g_dpram[(a - 0x140000) >> 1] << 8) | 0xff;
    if (a >= 0x200000 && a < 0x200020) return es_read((a >> 1) & 0xf);
    if (a >= 0x260000 && a < 0x260200) return 0xff00 | esp_read((a - 0x260000) >> 1);
    if (a >= 0x280000 && a < 0x280020) return 0xff00 | du_read((a - 0x280000) >> 1);
    return 0xffff;
}
uint32_t snd_read8(uint32_t a) {
    a &= 0xffffff;
    if (a < 0x40000 || a >= 0xff0000) return sram[a & 0xffff];
    if (a >= 0xc00000 && a < 0xc80000) return prog[a - 0xc00000];
    uint32_t w = snd_read16(a);
    return (a & 1) ? (w & 0xff) : (w >> 8);
}
static void snd_write16m(uint32_t a, uint16_t d, uint16_t mask) {
    a &= 0xfffffe;
    if (a < 0x40000 || a >= 0xff0000) {
        if (mask & 0xff00) sram[a & 0xffff] = d >> 8;
        if (mask & 0x00ff) sram[(a & 0xffff) + 1] = (uint8_t)d;
        return;
    }
    if (a >= 0x140000 && a < 0x141000) { if (mask & 0xff00) g_dpram[(a - 0x140000) >> 1] = d >> 8; return; }
    if (a >= 0x200000 && a < 0x200020) { es_write((a >> 1) & 0xf, d, mask); return; }
    if (a >= 0x260000 && a < 0x260200) { if (mask & 0xff) esp_write((a - 0x260000) >> 1, (uint8_t)d); return; }
    if (a >= 0x280000 && a < 0x280020) { if (mask & 0xff) du_write((a - 0x280000) >> 1, (uint8_t)d); return; }
    if (a >= 0x300000 && a < 0x300040) { int i = (a - 0x300000) >> 1; otisbank[i] = (otisbank[i] & ~mask) | (d & mask); es_sync(cur_time()); return; }
    if (a >= 0x340000 && a < 0x340004) { if (mask & 0xff00) mb_write(((a >> 1) & 1) ^ 1, d >> 8); return; }
}
void snd_write16(uint32_t a, uint32_t v) { snd_write16m(a, (uint16_t)v, 0xffff); }
void snd_write8(uint32_t a, uint32_t v) {
    if (a & 1) snd_write16m(a, (uint16_t)(v & 0xff), 0x00ff);
    else       snd_write16m(a, (uint16_t)((v & 0xff) << 8), 0xff00);
}

/* ------------------------------------------------------------------ public */
void snd_init(const uint8_t *p, uint32_t psize, const uint8_t *samples, uint32_t ssize) {
    (void)psize;
    prog = p; srom = samples; srom_words_mask = ssize / 2 - 1;
    /* ES5505 volume table: compute_tables(8, 4, 4) */
    for (int i = 0; i < 256; i++) {
        uint32_t exponent = (i >> 4) & 15, mantissa = (i & 15) | 16;
        vol_lookup[i] = (mantissa << 11) >> (16 - exponent);
    }
}
void snd_reset(void) {
    memset(sram, 0, sizeof sram);
    memcpy(sram, prog, 8);                /* stack + reset vectors (taito_en device_reset) */
    memset(ev, 0, sizeof ev);
    for (int i = 0; i < 32; i++) { ev[i].control = C_STOPMASK; ev[i].lvol = ev[i].rvol = 0x80; }
    es_page = 0; es_active = 0x1f; es_irqv = 0x80; es_mode = 0;
    memset(otisbank, 0, sizeof otisbank);
    for (int i = 0; i < 4; i++) { mb_latch0[i] = 0x3f; mb_latch1[i] = 0x04; mb_update(i); }
    mb_ch = 0;
    du_acr = du_imr = du_isr = du_ctur = du_ctlr = du_opcr = du_half = 0; du_ivr = 0x0f; du_running = 0;
    memset(esp_gpr, 0, sizeof esp_gpr); memset(esp_instr, 0, sizeof esp_instr);
    esp_gpr_latch = esp_dil = esp_dol = esp_dadr = 0; esp_instr_latch = 0; esp_host_ctrl = esp_ram_sel = esp_serial = 0;
    snd_now = frame_base = 0; es_next = 0;
    out_w = out_r = out_count = 0; rs_pos = 0; rs_prevL = rs_prevR = rs_curL = rs_curR = 0;
    cpu_snd_reset();
    cpu_snd_set_irq(0);
    in_reset = 1;
}
void snd_set_reset_line(int asserted) {
    if (asserted) { in_reset = 1; return; }
    if (in_reset) {
        in_reset = 0;
        du_acr = du_imr = du_isr = du_half = 0; du_ivr = 0x0f; du_running = 0;
        cpu_snd_set_irq(0);
        cpu_snd_reset();
    }
}

void snd_run_until(int64_t main_cycle) {
    int64_t target = frame_base + main_cycle * SND_CYCLES_PER_FRAME / CYCLES_PER_FRAME;
    while (snd_now < target) {
        int64_t stop = target;
        if (!in_reset && du_running && du_next < stop) stop = du_next;
        if (stop > snd_now + 2048) stop = snd_now + 2048;
        if (in_reset) {
            snd_now = stop;
        } else {
            slice_active = 1; slice_start = snd_now;
            int used = cpu_snd_execute((int)(stop - snd_now));
            slice_active = 0;
            snd_now += used > 0 ? used : (stop - snd_now);
        }
        es_sync(snd_now);
        while (!in_reset && du_running && du_next <= snd_now) du_fire();
    }
}
void snd_frame_end(void) {
    frame_base += SND_CYCLES_PER_FRAME;
}

int core_audio_samples(int16_t *out, int max_frames) {
    int n = 0;
    while (n < max_frames && out_count > 0) {
        out[n * 2] = outbuf[out_r * 2]; out[n * 2 + 1] = outbuf[out_r * 2 + 1];
        out_r = (out_r + 1) % OUTBUF; out_count--; n++;
    }
    return n;
}

uint32_t snd_state_hash(uint32_t h) {
    for (int i = 0; i < 0x10000; i++) h = (h ^ sram[i]) * 16777619u;
    for (int v = 0; v < 32; v++) h = (h ^ (uint32_t)ev[v].accum ^ ev[v].control) * 16777619u;
    h = (h ^ (uint32_t)snd_now) * 16777619u;
    return h;
}

void snd_state(sstream_t *s) {
    ss_io(s, sram, sizeof sram);
    SS(s, snd_now); SS(s, frame_base); SS(s, in_reset);
    for (int i = 0; i < 32; i++) {
        esvoice_t *v = &ev[i];
        SS(s, v->control); SS(s, v->freqcount); SS(s, v->start); SS(s, v->end); SS(s, v->accum);
        SS(s, v->lvol); SS(s, v->rvol); SS(s, v->k1); SS(s, v->k2);
        SS(s, v->o4n1); SS(s, v->o3n1); SS(s, v->o3n2); SS(s, v->o2n1); SS(s, v->o2n2); SS(s, v->o1n1);
    }
    SS(s, es_page); SS(s, es_active); SS(s, es_irqv); SS(s, es_mode); SS(s, otisbank); SS(s, es_next);
    SS(s, mb_latch0); SS(s, mb_latch1); SS(s, mb_ch);
    SS(s, du_acr); SS(s, du_imr); SS(s, du_isr); SS(s, du_ivr); SS(s, du_ctur); SS(s, du_ctlr); SS(s, du_opcr);
    SS(s, du_half); SS(s, du_running); SS(s, du_next);
    SS(s, esp_gpr); SS(s, esp_gpr_latch); SS(s, esp_dil); SS(s, esp_dol); SS(s, esp_dadr);
    SS(s, esp_instr); SS(s, esp_instr_latch); SS(s, esp_host_ctrl); SS(s, esp_ram_sel); SS(s, esp_serial);
    SS(s, rs_pos); SS(s, rs_prevL); SS(s, rs_prevR); SS(s, rs_curL); SS(s, rs_curR);
    if (s->loading && !s->error) {
        for (int i = 0; i < 4; i++) mb_update(i);
        out_w = out_r = out_count = 0;    /* queued host audio is dropped on load */
    }
}
