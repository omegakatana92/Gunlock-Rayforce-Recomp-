/* support header for generated recompiled code */
#ifndef RC_H
#define RC_H
#include "core.h"
#include "m68kcpu.h"
#define COND_T() 1
#define COND_F() 0
static inline int rc_mulu_cyc(uint32_t x) { int c = 0; for (uint32_t y = x & 0xffff; y; y >>= 1) if (y & 1) c += 2; return c; }
static inline int rc_muls_cyc(uint32_t x16) {
    int c = 0; uint32_t f = 0;
    for (uint32_t y = (uint32_t)(int32_t)(int16_t)x16; y; y >>= 1) if ((y & 1) != f) { c += 2; f = 1 - f; }
    return c;
}
typedef uint32_t (*rc_chunk_fn)(uint32_t pc);
extern rc_chunk_fn rc_chunk_table[2048];
uint32_t rc_interp_fallback(uint32_t pc, int from_dispatch);
void rc_trace(uint32_t pc);
#endif
