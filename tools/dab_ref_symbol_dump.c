#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab.h"

typedef struct {
    char magic[8];
    uint32_t symbol_count;
    int32_t softbits[NFIC][2][KSYM1];
} ref_fic_symbol_dump_t;

static const char *default_path(void) {
    const char *env = getenv("DAB_REF_REPLAY_PATH");

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return "/tmp/samples.bin";
}

static unsigned long env_u32(const char *name, unsigned long fallback) {
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static int write_binary_file(const char *path, const void *data, size_t size) {
    FILE *fp;

    if (path == NULL || path[0] == '\0' || data == NULL) {
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    if (fwrite(data, 1u, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

void unflatten_tframe(tframe_t *tframe, tframe_raw_t *tframe_raw);
void bytes_to_bits(bit_t *data, bit_t *abit, bit_t *bbit);
void debug_ref_set_capture_enabled(int enabled);
void debug_ref_reset_capture_state(void);

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : default_path();
    const char *dump_path = argc > 2 ? argv[2] : "/tmp/ref_fic_symbols.bin";
    FILE *fp = NULL;
    sdr_context_t ctx_handle = NULL;
    SdrContext_t *ctx = NULL;
    context_t context = {0};
    softbit_complex_t buf[TCPSYM];
    bit_t abit[KSYM1];
    bit_t bbit[KSYM1];
    ref_fic_symbol_dump_t dump = { { 'D', 'A', 'B', 'R', 'E', 'F', '1', '\0' }, 0u, {{{0}}} };
    unsigned long frames = 0;
    unsigned long valid_frames = 0;
    unsigned long stop_after_valid_frames = env_u32("DAB_REF_STOP_AFTER_VALID_FRAMES", 1ul);
    unsigned long enable_capture_after_valid_frames =
        env_u32("DAB_REF_ENABLE_CAPTURE_AFTER_VALID_FRAMES", 0ul);
    int rc = 1;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    ctx_handle = sdrplay_api_init(NULL);
    if (ctx_handle == NULL) {
        fprintf(stderr, "ref symbol dump init failed\n");
        goto cleanup;
    }
    ctx = (SdrContext_t *)ctx_handle;
    debug_ref_reset_capture_state();
    debug_ref_set_capture_enabled(enable_capture_after_valid_frames == 0ul ? 1 : 0);

    while (fread(buf, sizeof(buf[0]), TCPSYM, fp) == TCPSYM) {
        unsigned char valid = 0;
        int frame_idx;
        int i;

        memset(abit, 0, sizeof(abit));
        memset(bbit, 0, sizeof(bbit));

        frame_idx = rx_samples(NULL, &context, TCPSYM, buf, abit, bbit, &valid);
        frames++;
        if (!valid) {
            continue;
        }

        bytes_to_bits(ctx->tframe_raw.frame[frame_idx - 1].data, abit, bbit);

        if ((unsigned int)frame_idx <= NFIC && dump.symbol_count < NFIC) {
            for (i = 0; i < KSYM1; ++i) {
                dump.softbits[frame_idx - 1][0][i] = abit[i];
                dump.softbits[frame_idx - 1][1][i] = bbit[i];
            }
            if (dump.symbol_count < (uint32_t)frame_idx) {
                dump.symbol_count = (uint32_t)frame_idx;
            }
        }

        if (frame_idx == NTFRAME) {
            unflatten_tframe(ctx->tframe, &ctx->tframe_raw);
            valid_frames++;
            if (valid_frames >= enable_capture_after_valid_frames) {
                debug_ref_set_capture_enabled(1);
            }
            if (dump.symbol_count >= NFIC && valid_frames >= stop_after_valid_frames) {
                break;
            }
        }
    }

    if (write_binary_file(dump_path, &dump, sizeof(dump)) != 0) {
        fprintf(stderr, "failed to write %s\n", dump_path);
        goto cleanup;
    }

    printf("ref-symbol-dump frames=%lu valid_frames=%lu symbol_count=%u dump=%s\n",
           frames,
           valid_frames,
           dump.symbol_count,
           dump_path);
    rc = 0;

cleanup:
    if (ctx_handle != NULL) {
        sdrplay_api_shutdown(ctx_handle);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return rc;
}
