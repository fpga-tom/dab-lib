#include <stdio.h>
#include <stdlib.h>

#include "dab_api.h"

static const char *default_path(void) {
    const char *env = getenv("DAB_REF_REPLAY_PATH");

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return "/tmp/samples.bin";
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : default_path();
    FILE *fp = NULL;
    sdr_context_t ctx = NULL;
    softbit_complex_t buf[2552];
    unsigned long frames = 0;
    unsigned long valid_frames = 0;
    int i;
    int rc = 1;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    ctx = sdrplay_api_init(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "ref-impl init failed\n");
        goto cleanup;
    }

    while (fread(buf, sizeof(buf[0]), 2552u, fp) == 2552u) {
        int valid = process_data(ctx, buf, 2552u);
        frames++;
        if (valid) {
            valid_frames++;
        }
    }

    printf("ref-replay frames=%lu valid_frames=%lu max_channels=%d\n",
           frames,
           valid_frames,
           max_channels());
    for (i = 0; i < max_channels(); ++i) {
        char label[17] = {0};
        int valid = is_subch_valid(ctx, i);

        if (valid && subch_to_label(ctx, i, label)) {
            printf("subch=%d valid=1 label=%s\n", i, label);
        } else if (valid) {
            printf("subch=%d valid=1 label=\n", i);
        }
    }

    rc = 0;

cleanup:
    if (ctx != NULL) {
        sdrplay_api_shutdown(ctx);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return rc;
}
