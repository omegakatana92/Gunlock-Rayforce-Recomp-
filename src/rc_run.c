/* top-level dispatcher for the recompiled program */
#include "rc.h"
#define CHKBIT 0x80000000u
void cpu_take_pending_irq(void);
int rc_has(uint32_t pc) { return rc_chunk_table[(pc & 0xffffff) >> 13] != 0; }
void rc_run(void) {
    uint32_t r = REG_PC & 0xffffff;
    for (;;) {
        if ((r & CHKBIT) && g_cycles >= g_next_event) {
            REG_PC = r & 0xffffff; core_service(); r = REG_PC & 0xffffff;
        }
        if (g_frame_done) break;
        if (CPU_STOPPED) {
            REG_PC = r & 0xffffff;
            if (g_next_event > g_cycles) g_cycles = g_next_event;
            core_service(); r = REG_PC & 0xffffff; continue;
        }
        uint32_t pc = r & 0xffffff;
        rc_chunk_fn f = rc_chunk_table[pc >> 13];
        r = f ? f(pc) : rc_interp_fallback(pc, 1);
    }
    REG_PC = r & 0xffffff;
}
