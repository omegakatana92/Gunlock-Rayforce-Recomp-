/* explicit machine-state serialization (save states / rewind). Taken only between frames. */
#ifndef GL_STATE_H
#define GL_STATE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
typedef struct { uint8_t *p; size_t pos, cap; int loading; int error; } sstream_t;
static inline void ss_io(sstream_t *s, void *d, size_t n) {
    if (s->error) return;
    if (s->loading) {
        if (s->pos + n > s->cap) { s->error = 1; return; }
        memcpy(d, s->p + s->pos, n);
    } else {
        if (s->pos + n > s->cap) {
            size_t nc = s->cap ? s->cap * 2 : 1 << 20;
            while (nc < s->pos + n) nc *= 2;
            uint8_t *np = (uint8_t *)realloc(s->p, nc);
            if (!np) { s->error = 1; return; }
            s->p = np; s->cap = nc;
        }
        memcpy(s->p + s->pos, d, n);
    }
    s->pos += n;
}
#define SS(s, v) ss_io((s), &(v), sizeof(v))
void hw_state(sstream_t *s);
void snd_state(sstream_t *s);
void video_state(sstream_t *s);
void cpu_state(sstream_t *s);
#endif
