/* K=7 r=1/4 Viterbi decoder in portable C
 * Imported from the reference project's viterbi47_port.c and adapted only
 * to use a local parity helper/header.
 */
#include "viterbi47_port.h"

#include <limits.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#define V47POLYA 109
#define V47POLYB 79
#define V47POLYC 83
#define V47POLYD 109

typedef union { unsigned int w[64]; } metric_t;
typedef union { unsigned long w[2]; } decision_t;
#pragma pack(push, 16)
static union branchtab47 { unsigned char c[32]; } Branchtab47[4];
#pragma pack(pop)
static int Init = 0;

struct v47 {
    metric_t metrics1;
    metric_t metrics2;
    decision_t *dp;
    metric_t *old_metrics;
    metric_t *new_metrics;
    decision_t *decisions;
};

static inline int dab_parityb(unsigned char x) {
    x ^= (unsigned char)(x >> 4);
    x ^= (unsigned char)(x >> 2);
    x ^= (unsigned char)(x >> 1);
    return x & 1u;
}

static inline int dab_parity(int x) {
    x ^= (x >> 16);
    x ^= (x >> 8);
    return dab_parityb((unsigned char)x);
}

int init_viterbi47_port(void *p, int starting_state) {
    struct v47 *vp = p;
    int i;

    for (i = 0; i < 64; i++) {
        vp->metrics1.w[i] = 63;
    }

    vp->old_metrics = &vp->metrics1;
    vp->new_metrics = &vp->metrics2;
    vp->dp = vp->decisions;
    vp->old_metrics->w[starting_state & 63] = 0;
    return 0;
}

void *create_viterbi47_port(int len) {
    struct v47 *vp;
    int state;

    if (!Init) {
        for (state = 0; state < 32; state++) {
            Branchtab47[0].c[state] = dab_parity((2 * state) & V47POLYA) ? 255 : 0;
            Branchtab47[1].c[state] = dab_parity((2 * state) & V47POLYB) ? 255 : 0;
            Branchtab47[2].c[state] = dab_parity((2 * state) & V47POLYC) ? 255 : 0;
            Branchtab47[3].c[state] = dab_parity((2 * state) & V47POLYD) ? 255 : 0;
        }
        Init++;
    }
    vp = malloc(sizeof(struct v47));
    if (vp == 0) {
        return 0;
    }
    vp->decisions = malloc((size_t)(len + 6) * sizeof(decision_t));
    if (vp->decisions == 0) {
        free(vp);
        return 0;
    }
    init_viterbi47_port(vp, 0);

    return vp;
}

int chainback_viterbi47_port(void *p,
                             unsigned char *data,
                             unsigned int nbits,
                             unsigned int endstate) {
    struct v47 *vp = p;
    decision_t *d = vp->decisions;

    endstate %= 64u;
    endstate <<= 2u;

    d += 6;
    while (nbits-- != 0u) {
        int k;

        k = (int)((d[nbits].w[(endstate >> 2u) / 32u] >> ((endstate >> 2u) % 32u)) & 1u);
        data[nbits >> 3u] = (unsigned char)(endstate = (endstate >> 1u) | ((unsigned int)k << 7u));
    }
    return 0;
}

void delete_viterbi47_port(void *p) {
    struct v47 *vp = p;

    if (vp != 0) {
        free(vp->decisions);
        free(vp);
    }
}

#define BFLY(i) { \
unsigned int metric, m0, m1, decision; \
    metric = (Branchtab47[0].c[i] ^ sym0) + (Branchtab47[1].c[i] ^ sym1) \
           + (Branchtab47[2].c[i] ^ sym2) + (Branchtab47[3].c[i] ^ sym3); \
    m0 = vp->old_metrics->w[i] + metric; \
    m1 = vp->old_metrics->w[i + 32] + (1020 - metric); \
    decision = (signed int)(m0 - m1) > 0; \
    vp->new_metrics->w[2 * i] = decision ? m1 : m0; \
    d->w[i / 16] |= decision << ((2 * i) & 31); \
    m0 -= (metric + metric - 1020); \
    m1 += (metric + metric - 1020); \
    decision = (signed int)(m0 - m1) > 0; \
    vp->new_metrics->w[2 * i + 1] = decision ? m1 : m0; \
    d->w[i / 16] |= decision << ((2 * i + 1) & 31); \
}

int update_viterbi47_blk_port(void *p, unsigned char *syms, int nbits) {
    struct v47 *vp = p;
    void *tmp;
    decision_t *d = (decision_t *)vp->dp;

    while (nbits--) {
        unsigned char sym0, sym1, sym2, sym3;

        d->w[0] = d->w[1] = 0;
        sym0 = *syms++;
        sym1 = *syms++;
        sym2 = *syms++;
        sym3 = *syms++;

        BFLY(0);  BFLY(1);  BFLY(2);  BFLY(3);
        BFLY(4);  BFLY(5);  BFLY(6);  BFLY(7);
        BFLY(8);  BFLY(9);  BFLY(10); BFLY(11);
        BFLY(12); BFLY(13); BFLY(14); BFLY(15);
        BFLY(16); BFLY(17); BFLY(18); BFLY(19);
        BFLY(20); BFLY(21); BFLY(22); BFLY(23);
        BFLY(24); BFLY(25); BFLY(26); BFLY(27);
        BFLY(28); BFLY(29); BFLY(30); BFLY(31);
        d++;
        tmp = vp->old_metrics;
        vp->old_metrics = vp->new_metrics;
        vp->new_metrics = tmp;
    }
    vp->dp = d;
    return 0;
}
