#include "dab/core/frontend.h"

#include "dab/simd/simd.h"

void dab_frontend_default_config(dab_frontend_config_t *config) {
    if (config == 0) {
        return;
    }
    config->dc_alpha = 0.001f;
}

int dab_frontend_init(dab_frontend_t *frontend, const dab_frontend_config_t *config) {
    dab_frontend_config_t defaults;

    if (frontend == 0) {
        return -1;
    }

    dab_frontend_default_config(&defaults);
    frontend->config = config != 0 ? *config : defaults;
    if (frontend->config.dc_alpha <= 0.0f || frontend->config.dc_alpha > 1.0f) {
        return -1;
    }

    dab_frontend_reset(frontend);
    return 0;
}

void dab_frontend_reset(dab_frontend_t *frontend) {
    if (frontend == 0) {
        return;
    }
    frontend->dc_i = 0.0f;
    frontend->dc_q = 0.0f;
    frontend->processed_samples = 0u;
}

static void dab_frontend_remove_dc(dab_frontend_t *frontend, dab_frontend_block_t *dst) {
    uint32_t i;
    float mean_i = frontend->dc_i;
    float mean_q = frontend->dc_q;
    const float alpha = frontend->config.dc_alpha;

    for (i = 0u; i < dst->sample_count; ++i) {
        mean_i += alpha * (dst->i_data[i] - mean_i);
        mean_q += alpha * (dst->q_data[i] - mean_q);
        dst->i_data[i] -= mean_i;
        dst->q_data[i] -= mean_q;
    }

    frontend->dc_i = mean_i;
    frontend->dc_q = mean_q;
}

int dab_frontend_process(dab_frontend_t *frontend,
                         const dab_sdr_block_view_t *src,
                         dab_frontend_block_t *dst) {
    const dab_simd_ops_t *simd;

    if (frontend == 0 || src == 0 || dst == 0) {
        return -1;
    }
    if (src->iq_interleaved == 0 || dst->i_data == 0 || dst->q_data == 0) {
        return -1;
    }

    simd = dab_simd_get_ops();
    simd->complex_i16_to_f32(dst->i_data, dst->q_data, src->iq_interleaved, src->sample_count);

    dst->sample_index = src->sample_index;
    dst->timestamp_ns = src->timestamp_ns;
    dst->sample_count = src->sample_count;

    dab_frontend_remove_dc(frontend, dst);
    frontend->processed_samples += src->sample_count;
    return 0;
}
