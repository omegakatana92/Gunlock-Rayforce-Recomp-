/* headless test harness: runs N frames, dumps screenshots / per-frame state hashes */
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

static void save_png(const char *path, const uint32_t *fb) {
    static uint8_t rgb[SCREEN_W * SCREEN_H * 3];
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) { rgb[i*3] = fb[i] >> 16; rgb[i*3+1] = fb[i] >> 8; rgb[i*3+2] = fb[i]; }
    size_t len; void *png = tdefl_write_image_to_png_file_in_memory(rgb, SCREEN_W, SCREEN_H, 3, &len);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(png, 1, len, f); fclose(f); }
    mz_free(png);
}

/* deterministic scripted input: coin, start, then play */
static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static void script_input(uint64_t f) {
    g_input[0] = g_input[1] = 0;
    if (getenv("RANDOM_PLAY")) {
        static uint16_t held; static int hold_left;
        uint64_t ph = f % 4000;
        if (getenv("TESTMODE") && f > 300 && f < 320) g_input[0] |= IN_TEST;
        if (getenv("TESTMODE") && f > 320 && f < 3000) { if (rnd() % 20 == 0) g_input[0] |= (rnd() & 1) ? IN_DOWN : IN_UP; if (rnd() % 60 == 0) g_input[0] |= IN_B1; if (rnd() % 200 == 0) g_input[0] |= IN_START; return; }
        if (ph >= 100 && ph < 110) g_input[0] |= IN_COIN;
        if (ph >= 120 && ph < 130) g_input[1] |= IN_COIN;
        if (ph >= 200 && ph < 210) g_input[0] |= IN_START;
        if (getenv("TWO_PLAYER") && ph >= 230 && ph < 240) g_input[1] |= IN_START;
        if (ph >= 300 && ph < 400 && (f / 4000) % 3 == 1) g_input[0] |= IN_RIGHT;
        if (ph >= 300 && ph < 400 && (f / 4000) % 3 == 2) g_input[0] |= IN_LEFT;
        if (ph >= 420 && ph < 430) g_input[0] |= IN_B1, g_input[1] |= IN_B1;
        if (hold_left-- <= 0) { held = (uint16_t)(rnd() & 0x0f); if ((held & 3) == 3) held &= ~2; if ((held & 12) == 12) held &= ~8; hold_left = rnd() % 40; }
        g_input[0] |= held; g_input[1] |= (held ^ 0x5) & 0xf;
        if (rnd() % 3) g_input[0] |= IN_B1, g_input[1] |= IN_B1;
        if (rnd() % 400 == 0) g_input[0] |= IN_B2;
        if (rnd() % 7 == 0) g_input[0] |= IN_B3;
        if (ph >= 3000 && ph < 3010) g_input[0] |= IN_START;  /* continue */
        return;
    }
    static int coin_at = -1; if (coin_at < 0) coin_at = getenv("COIN_AT") ? atoi(getenv("COIN_AT")) : 600;
    uint64_t c = coin_at;
    if (f >= c && f < c + 10) g_input[0] |= IN_COIN;
    if (f >= c + 100 && f < c + 110) g_input[0] |= IN_START;
        if (f >= c + 400) {
        g_input[0] |= ((f / 8) % 2) ? IN_B1 : 0;
        uint64_t ph = (f / 90) % 4;
        g_input[0] |= ph == 0 ? IN_LEFT : ph == 1 ? IN_UP : ph == 2 ? IN_RIGHT : IN_DOWN;
        if ((f % 600) == 300) g_input[0] |= IN_B2;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s romset frames interp|recomp [shotdir] [shotevery] [hashfile] [covfile]\n", argv[0]); return 1; }
    int frames = atoi(argv[2]);
    cpu_mode_t mode = !strcmp(argv[3], "interp") ? CPU_MODE_INTERP : CPU_MODE_RECOMP;
    const char *shotdir = argc > 4 ? argv[4] : NULL;
    int every = argc > 5 ? atoi(argv[5]) : 100;
    FILE *hf = argc > 6 && strcmp(argv[6], "-") ? fopen(argv[6], "w") : NULL;
    if (argc > 7) cpu_coverage_enable(argv[7]);
    if (getenv("TRACE_UNKNOWN")) g_trace_unknown = 1;
    if (!core_load_romset(argv[1])) { fprintf(stderr, "romset load failed: %s\n", core_last_error()); return 1; }
    core_reset();
    if (getenv("STATE_TEST")) {
        int at = atoi(getenv("STATE_TEST"));
        uint8_t *blob = NULL; size_t bn = 0; uint32_t ha[4096]; int cnt = frames - at; if (cnt > 4096) cnt = 4096;
        for (int f = 0; f < at; f++) { script_input(f); core_run_frame(mode); }
        bn = core_state_save(&blob);
        static uint16_t rec[4096][2];
        for (int i = 0; i < cnt; i++) { script_input(at + i); rec[i][0] = g_input[0]; rec[i][1] = g_input[1]; core_run_frame(mode); ha[i] = core_state_hash(); }
        if (!core_state_load(blob, bn)) { fprintf(stderr, "load failed\n"); return 2; }
        int bad = 0;
        for (int i = 0; i < cnt; i++) { g_input[0] = rec[i][0]; g_input[1] = rec[i][1]; core_run_frame(mode); if (core_state_hash() != ha[i]) { bad++; if (bad < 3) fprintf(stderr, "mismatch at +%d\n", i); } }
        /* corrupt blob must be rejected and leave state intact */
        uint32_t before = core_state_hash();
        int r = core_state_load(blob, bn - 100);
        fprintf(stderr, "state size %zu bytes, replay mismatches %d/%d, truncated load rejected=%d intact=%d\n", bn, bad, cnt, !r, before == core_state_hash());
        return bad ? 1 : 0;
    }
    long long audio_frames = 0, nonzero = 0;
    int16_t abuf[8192];
    int f0 = 0;
    if (getenv("LOAD_FILE")) {
        FILE *lf = fopen(getenv("LOAD_FILE"), "rb"); static uint8_t buf[4 << 20]; size_t n = fread(buf, 1, sizeof buf, lf); fclose(lf);
        if (getenv("CORRUPT")) buf[atoi(getenv("CORRUPT"))] ^= 0x5a;
        int ok = core_state_load(buf, n); fprintf(stderr, "load %s: %s\n", getenv("LOAD_FILE"), ok ? "OK" : "REJECTED");
        if (!ok) return 3;
        f0 = atoi(getenv("START_FRAME"));
    }
    for (int f = f0; f < frames; f++) {
        if (getenv("SAVE_FILE") && f == atoi(getenv("SAVE_AT"))) { uint8_t *b; size_t n = core_state_save(&b); FILE *sf = fopen(getenv("SAVE_FILE"), "wb"); fwrite(b, 1, n, sf); fclose(sf); free(b); }
        script_input(f);
        core_run_frame(mode);
        int n; while ((n = core_audio_samples(abuf, 4096)) > 0) { audio_frames += n; for (int i = 0; i < n * 2; i++) if (abuf[i]) nonzero++;
            static FILE *wf; if (!wf && getenv("WAV")) { wf = fopen(getenv("WAV"), "wb"); fseek(wf, 44, SEEK_SET); }
            if (wf) { fwrite(abuf, 4, n, wf); long sz = ftell(wf); uint32_t hdr[11] = {0x46464952, (uint32_t)sz - 8, 0x45564157, 0x20746d66, 16, 0x00020001, AUDIO_RATE, AUDIO_RATE * 4, 0x00100004, 0x61746164, (uint32_t)sz - 44}; fseek(wf, 0, SEEK_SET); fwrite(hdr, 4, 11, wf); fseek(wf, sz, SEEK_SET); } }
        if (getenv("TRACE_PC") && f % atoi(getenv("TRACE_PC")) == 0) { char b[512]; void cpu_debug_regs(char*, size_t); uint32_t cpu_snd_pc(void); cpu_debug_regs(b, sizeof b); fprintf(stderr, "f%d %s snd=%06x\n", f, b, cpu_snd_pc()); }
        if (hf) { extern uint8_t g_ram[]; uint32_t h = 2166136261u; for (int i = 0; i < 0x20000; i++) h = (h ^ g_ram[i]) * 16777619u; fprintf(hf, "%d %08x ram=%08x cyc=%lld\n", f, core_state_hash(), h, (long long)g_cycles); }
        if (shotdir && every > 0 && (f + 1) % every == 0) {
            char p[512]; snprintf(p, sizeof p, "%s/f%05d.png", shotdir, f + 1); save_png(p, core_framebuffer());
        }
    }
    if (getenv("DUMP_TEXT")) {
        for (int r = 0; r < 64; r++) { char line[65]; int any = 0;
            for (int c = 0; c < 64; c++) { int t = g_textram[(r * 64 + c) * 2 + 1]; line[c] = (t >= 32 && t < 127) ? t : (t ? '?' : '.'); if (t) any = 1; }
            line[64] = 0; if (any) fprintf(stderr, "%02d %s\n", r, line); }
    }
    if (hf) fclose(hf);
    cpu_coverage_flush();
    fprintf(stderr, "done %d frames, audio frames %lld (nonzero samples %lld)\n", frames, audio_frames, nonzero);
    return 0;
}
