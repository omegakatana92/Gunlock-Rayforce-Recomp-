/* Gunlock recomp - CPU glue.
 * Main CPU: 68EC020. Its program is statically recompiled (gen/*.c); Musashi executes any
 *           instruction the recompiler leaves to the interpreter and serves as the reference core.
 * Sound CPU: 68000 on the Taito EN board, run through a second Musashi context (sound.c).
 * Musashi has one global register file, so the two CPUs swap contexts (592 bytes) around each
 * sound time-slice. Recompiled code always runs with the main context loaded.
 */
#include "core.h"
#include "m68kcpu.h"
#include "m68kops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "state.h"

int g_cpu_active;                 /* 0 = main 68EC020, 1 = sound 68000 */

/* sound-side bus (sound.c) */
uint32_t snd_read8(uint32_t a);
uint32_t snd_read16(uint32_t a);
void snd_write8(uint32_t a, uint32_t v);
void snd_write16(uint32_t a, uint32_t v);
int snd_int_ack(int level);

/* Musashi memory callbacks */
unsigned int m68k_read_memory_8(unsigned int a)  { return g_cpu_active ? snd_read8(a) : rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return g_cpu_active ? snd_read16(a) : rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) {
    if (g_cpu_active) return (snd_read16(a) << 16) | snd_read16(a + 2);
    return rd32(a);
}
void m68k_write_memory_8(unsigned int a, unsigned int v)  { if (g_cpu_active) snd_write8(a, v); else wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { if (g_cpu_active) snd_write16(a, v); else wr16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (g_cpu_active) { snd_write16(a, v >> 16); snd_write16(a + 2, v & 0xffff); }
    else wr32(a, v);
}
unsigned int m68k_read_disassembler_8(unsigned int a)  { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

static uint8_t is_check[0x10000];   /* branch-class / SR-changing: interrupt check points */
static uint8_t is_sr[0x10000];
static int irq_hold;                 /* main CPU: bitmask of asserted HOLD_LINE levels */
int g_trace_unknown;

static uint8_t *cov_landing;          /* landing pcs (targets of non-sequential flow) */
static const char *cov_path;
static int cov_prev_check = 1;

/* contexts */
static uint8_t main_ctx[1024], snd_ctx[1024];

static int classify_check(uint32_t op) {
    if ((op & 0xf000) == 0x6000) return 1;               /* bra/bsr/bcc */
    if ((op & 0xf0f8) == 0x50c8) return 1;               /* dbcc */
    if ((op & 0xffc0) == 0x4ec0 || (op & 0xffc0) == 0x4e80) return 1; /* jmp/jsr */
    if (op == 0x4e70 || op == 0x4e72 || op == 0x4e73 || op == 0x4e74 || op == 0x4e75 || op == 0x4e76 || op == 0x4e77) return 1;
    if ((op & 0xfff0) == 0x4e40) return 1;               /* trap */
    if ((op & 0xfff8) == 0x50f8 && (op & 7) >= 2) return 1; /* trapcc (020) */
    if ((op & 0xffc0) == 0x46c0) return 2;               /* move to sr */
    if (op == 0x027c || op == 0x007c || op == 0x0a7c) return 2; /* andi/ori/eori sr */
    return 0;
}

int gl_int_ack(int level) {
    if (g_cpu_active) return snd_int_ack(level);
    irq_hold &= ~(1 << level);        /* HOLD_LINE: cleared when acknowledged */
    return M68K_INT_ACK_AUTOVECTOR;
}

void cpu_init(void) {
    static int done;
    if (done) return;
    done = 1;
    m68k_init();
    /* sound context */
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_get_context(snd_ctx);
    /* main context stays loaded */
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    for (uint32_t op = 0; op < 0x10000; op++) {
        int c = classify_check(op);
        is_check[op] = c != 0;
        is_sr[op] = c == 2 || op == 0x4e73 || op == 0x4e77 || op == 0x4e72;
    }
}

void cpu_reset(void) {
    irq_hold = 0;
    /* sound CPU */
    g_cpu_active = 1;
    m68k_set_context(snd_ctx);
    m68k_pulse_reset();
    m68k_get_context(snd_ctx);
    /* main CPU */
    g_cpu_active = 0;
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    m68k_pulse_reset();
    m68k_get_context(main_ctx);
}

void cpu_set_irq_line(int level) { if (level) irq_hold |= 1 << level; }

static int pending_level(void) {
    for (int l = 7; l > 0; l--) if (irq_hold & (1 << l)) return l;
    return 0;
}

void cpu_take_pending_irq(void) {
    int lv = pending_level();
    if (lv && (uint32_t)lv > (FLAG_INT_MASK >> 8)) {
        m68ki_remaining_cycles = 1000000;
        m68ki_exception_interrupt(lv);
        g_cycles += 1000000 - m68ki_remaining_cycles;
        cov_prev_check = 1;
    }
}

/* execute one main-CPU instruction with Musashi. Used by interpreter mode and by
 * recompiled code for opcodes it does not translate natively. */
void cpu_exec_one(uint32_t pc, uint32_t op) {
    REG_PPC = pc;
    REG_PC = pc + 2;
    REG_IR = op;
    m68ki_remaining_cycles = 1000000;
    m68ki_instruction_jump_table[op]();
    int extra = 1000000 - m68ki_remaining_cycles;
    if (CPU_STOPPED || extra < -1000 || extra > 100000) extra = 0;
    g_cycles += CYC_INSTRUCTION[op] + extra;
    if (is_sr[op]) g_next_event = 0;
}

static FILE *trace_f; static long trace_from = -1, trace_to;
void rc_trace(uint32_t pc) {
    if (trace_from < 0) { const char *e = getenv("TRACE_FRAMES"); if (!e) { trace_from = 1L << 40; return; } sscanf(e, "%ld-%ld", &trace_from, &trace_to); trace_f = fopen(getenv("TRACE_OUT"), "w"); }
    if ((long)g_frame_count >= trace_from && (long)g_frame_count <= trace_to && trace_f) fprintf(trace_f, "%06x %lld\n", pc, (long long)g_cycles);
}
/* called from recompiled code when a pc has no native translation */
uint32_t rc_interp_fallback(uint32_t pc, int from_dispatch) {
    uint32_t op = rd16(pc);
    if (from_dispatch && cov_landing) cov_landing[(pc & 0xffffff) >> 1] = 1;
    if (from_dispatch && g_trace_unknown) fprintf(stderr, "[unknown entry] %06x\n", pc);
    { static int tr = -1; if (tr < 0) tr = getenv("RC_TRACE") != NULL; if (from_dispatch && tr) rc_trace(pc); }
    cpu_exec_one(pc, op);
    return REG_PC | (is_check[op] ? 0x80000000u : 0);
}

void cpu_interp_run(void) {
    while (!g_frame_done) {
        if (CPU_STOPPED) {
            if (g_next_event > g_cycles) g_cycles = g_next_event;
            core_service();
            continue;
        }
        uint32_t pc = REG_PC & 0xffffff;
        if (cov_landing && cov_prev_check) cov_landing[pc >> 1] = 1;
        uint32_t op = rd16(pc);
        { static uint32_t hist[64]; static int hp; static long bpc = -2;
          if (bpc == -2) { const char *e = getenv("BREAK_PC"); bpc = e ? strtol(e, 0, 16) : -1; }
          hist[hp++ & 63] = pc;
          if ((long)pc == bpc) { fprintf(stderr, "BREAK at %06x, history:\n", pc); for (int i = 0; i < 64; i++) fprintf(stderr, " %06x", hist[(hp + i) & 63]); fprintf(stderr, "\n"); char b[400]; void cpu_debug_regs(char*, size_t); cpu_debug_regs(b, sizeof b); fprintf(stderr, "%s\n", b); bpc = -1; } }
        { static int tr = -1; if (tr < 0) tr = getenv("RC_TRACE") != NULL; if (tr) rc_trace(pc); }
        cpu_exec_one(pc, op);
        cov_prev_check = is_check[op];
        if (is_check[op] && g_cycles >= g_next_event) core_service();
    }
}

/* ---- sound CPU execution (called by sound.c with a cycle budget) ---- */
extern int m68ki_initial_cycles;
int cpu_snd_execute(int cycles) {
    int save_rem = m68ki_remaining_cycles, save_init = m68ki_initial_cycles;
    m68k_get_context(main_ctx);
    g_cpu_active = 1;
    m68k_set_context(snd_ctx);
    int used = m68k_execute(cycles);
    m68k_get_context(snd_ctx);
    g_cpu_active = 0;
    m68k_set_context(main_ctx);
    m68ki_remaining_cycles = save_rem; m68ki_initial_cycles = save_init;
    return used;
}
void cpu_snd_set_irq(int level) {
    if (g_cpu_active) { m68k_set_irq(level); return; }   /* called from inside a sound slice */
    m68k_get_context(main_ctx);
    g_cpu_active = 1;
    m68k_set_context(snd_ctx);
    m68k_set_irq(level);
    m68k_get_context(snd_ctx);
    g_cpu_active = 0;
    m68k_set_context(main_ctx);
}
void cpu_snd_reset(void) {
    if (g_cpu_active) { m68k_pulse_reset(); return; }
    m68k_get_context(main_ctx);
    g_cpu_active = 1;
    m68k_set_context(snd_ctx);
    m68k_pulse_reset();
    m68k_get_context(snd_ctx);
    g_cpu_active = 0;
    m68k_set_context(main_ctx);
}
uint32_t cpu_snd_pc(void) { return ((m68ki_cpu_core *)snd_ctx)->pc; }
int cpu_snd_stopped(void) { return ((m68ki_cpu_core *)snd_ctx)->stopped; }

void cpu_coverage_enable(const char *path) {
    cov_path = path;
    cov_landing = calloc(0x800000, 1);
    FILE *f = fopen(path, "r");
    if (f) { unsigned a; while (fscanf(f, "%x", &a) == 1) cov_landing[(a & 0xffffff) >> 1] = 1; fclose(f); }
}
void cpu_coverage_flush(void) {
    if (!cov_landing) return;
    FILE *f = fopen(cov_path, "w");
    if (!f) return;
    for (uint32_t i = 0; i < 0x800000; i++) if (cov_landing[i]) fprintf(f, "%06x\n", i << 1);
    fclose(f);
}

static uint32_t hash_core(uint32_t h, const m68ki_cpu_core *c) {
    for (int i = 0; i < 16; i++) h = (h ^ c->dar[i]) * 16777619u;
    h = (h ^ c->pc) * 16777619u;
    return h;
}
uint32_t cpu_state_hash_regs(uint32_t h) {
    for (int i = 0; i < 16; i++) h = (h ^ REG_DA[i]) * 16777619u;
    h = (h ^ REG_PC) * 16777619u;
    h = (h ^ m68ki_get_sr()) * 16777619u;
    return hash_core(h, (const m68ki_cpu_core *)snd_ctx);
}
void cpu_debug_regs(char *buf, size_t n) {
    snprintf(buf, n, "PC=%06x SR=%04x D=%08x %08x %08x %08x %08x %08x %08x %08x A=%08x %08x %08x %08x %08x %08x %08x %08x",
        REG_PC, m68ki_get_sr(), REG_D[0], REG_D[1], REG_D[2], REG_D[3], REG_D[4], REG_D[5], REG_D[6], REG_D[7],
        REG_A[0], REG_A[1], REG_A[2], REG_A[3], REG_A[4], REG_A[5], REG_A[6], REG_A[7]);
}

/* Serialize the architectural part of a Musashi context: registers, flags, stack pointers,
 * stop state and pending-interrupt state. Pointers (cycle tables, callbacks) are rebuilt
 * from the cpu type and never written to a save state. */
static void ctx_state(sstream_t *s, m68ki_cpu_core *c) {
    char *base = (char *)c;
    size_t a0 = offsetof(m68ki_cpu_core, dar), a1 = offsetof(m68ki_cpu_core, ir) + sizeof(uint);
    size_t b0 = offsetof(m68ki_cpu_core, t1_flag), b1 = offsetof(m68ki_cpu_core, pref_data) + sizeof(uint);
    ss_io(s, base + a0, a1 - a0);
    ss_io(s, base + b0, b1 - b0);
    SS(s, c->int_level); SS(s, c->nmi_pending); SS(s, c->stopped); SS(s, c->cpu_type);
}
void cpu_state(sstream_t *s) {
    m68k_get_context(main_ctx);
    ctx_state(s, (m68ki_cpu_core *)main_ctx);
    ctx_state(s, (m68ki_cpu_core *)snd_ctx);
    if (s->loading && !s->error) m68k_set_context(main_ctx);
    SS(s, irq_hold);
}
