#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dab/core/fic.h"
#include "dab/core/tframe.h"

typedef enum {
    FIC_LAYOUT_PLANAR = 0,
    FIC_LAYOUT_INTERLEAVED = 1,
    FIC_LAYOUT_SWAPPED_HALVES = 2,
    FIC_LAYOUT_COUNT = 3
} fic_layout_t;

enum {
    FIC_SOFTBITS_PER_HALF_SYMBOL = DAB_TFRAME_SOFTBITS_PER_SYMBOL / 2u
};

typedef struct {
    fic_layout_t layout;
    dab_fic_debug_report_t normal;
    dab_fic_debug_report_t inverted;
    uint32_t union_valid_fibs;
} fic_layout_score_t;

static int read_fic_prefix(const char *path, int8_t *fic_softbits, long *file_size_out) {
    FILE *fp;
    long file_size;

    if (path == 0 || fic_softbits == 0) {
        return -1;
    }

    fp = fopen(path, "rb");
    if (fp == 0) {
        return -1;
    }
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    file_size = ftell(fp);
    if (file_size < 0 || file_size < (long)DAB_TFRAME_FIC_SOFTBITS) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    if (fread(fic_softbits, 1u, DAB_TFRAME_FIC_SOFTBITS, fp) != DAB_TFRAME_FIC_SOFTBITS) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (file_size_out != 0) {
        *file_size_out = file_size;
    }
    return 0;
}

static const char *layout_name(fic_layout_t layout) {
    switch (layout) {
        case FIC_LAYOUT_PLANAR: return "planar";
        case FIC_LAYOUT_INTERLEAVED: return "interleaved";
        case FIC_LAYOUT_SWAPPED_HALVES: return "swapped_halves";
        default: return "unknown";
    }
}

static void make_planar_copy(const int8_t *src, int8_t *dst) {
    memcpy(dst, src, sizeof(*dst) * DAB_TFRAME_FIC_SOFTBITS);
}

static void make_interleaved_copy(const int8_t *src, int8_t *dst) {
    uint32_t symbol;

    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        uint32_t i;
        const int8_t *src_sym = src + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        int8_t *dst_sym = dst + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);

        for (i = 0u; i < FIC_SOFTBITS_PER_HALF_SYMBOL; ++i) {
            dst_sym[2u * i] = src_sym[i];
            dst_sym[(2u * i) + 1u] = src_sym[FIC_SOFTBITS_PER_HALF_SYMBOL + i];
        }
    }
}

static void make_swapped_halves_copy(const int8_t *src, int8_t *dst) {
    uint32_t symbol;

    for (symbol = 0u; symbol < DAB_TFRAME_FIC_SYMBOLS; ++symbol) {
        uint32_t i;
        const int8_t *src_sym = src + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);
        int8_t *dst_sym = dst + (symbol * DAB_TFRAME_SOFTBITS_PER_SYMBOL);

        for (i = 0u; i < FIC_SOFTBITS_PER_HALF_SYMBOL; ++i) {
            dst_sym[i] = src_sym[FIC_SOFTBITS_PER_HALF_SYMBOL + i];
            dst_sym[FIC_SOFTBITS_PER_HALF_SYMBOL + i] = src_sym[i];
        }
    }
}

static void make_layout_copy(const int8_t *src, fic_layout_t layout, int8_t *dst) {
    switch (layout) {
        case FIC_LAYOUT_INTERLEAVED:
            make_interleaved_copy(src, dst);
            break;
        case FIC_LAYOUT_SWAPPED_HALVES:
            make_swapped_halves_copy(src, dst);
            break;
        case FIC_LAYOUT_PLANAR:
        default:
            make_planar_copy(src, dst);
            break;
    }
}

static void invert_softbits(int8_t *buf) {
    uint32_t i;

    for (i = 0u; i < DAB_TFRAME_FIC_SOFTBITS; ++i) {
        buf[i] = (int8_t)-buf[i];
    }
}

static uint32_t polarity0_pack0_valid_fibs(const dab_fic_debug_block_t *block) {
    if (block == 0 || block->polarity_variant != 0u || block->pack_variant != 0u) {
        return 0u;
    }
    return block->valid_fibs;
}

static uint32_t report_union_parseable_fibs(const dab_fic_debug_report_t *normal,
                                            const dab_fic_debug_report_t *inverted) {
    uint32_t total = 0u;
    uint32_t block_index;

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        const uint32_t a = polarity0_pack0_valid_fibs(&normal->blocks[block_index]);
        const uint32_t b = polarity0_pack0_valid_fibs(&inverted->blocks[block_index]);

        total += a > b ? a : b;
    }
    return total;
}

static int score_layout(const int8_t *src, fic_layout_t layout, fic_layout_score_t *score) {
    int8_t work[DAB_TFRAME_FIC_SOFTBITS];

    if (src == 0 || score == 0) {
        return -1;
    }

    memset(score, 0, sizeof(*score));
    score->layout = layout;

    make_layout_copy(src, layout, work);
    if (dab_fic_debug_analyze_frame(work, DAB_TFRAME_FIC_SOFTBITS, &score->normal) != 0) {
        return -1;
    }

    invert_softbits(work);
    if (dab_fic_debug_analyze_frame(work, DAB_TFRAME_FIC_SOFTBITS, &score->inverted) != 0) {
        return -1;
    }

    score->union_valid_fibs = report_union_parseable_fibs(&score->normal, &score->inverted);
    return 0;
}

static void print_report_line(const char *name, const dab_fic_debug_report_t *report) {
    uint32_t block_index;

    if (name == 0 || report == 0) {
        return;
    }

    printf("%s total_valid_fibs=%u", name, report->total_valid_fibs);
    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        const dab_fic_debug_block_t *block = &report->blocks[block_index];

        printf(" block%u=%u/pol%u/pack%u",
               block_index,
               block->valid_fibs,
               block->polarity_variant,
               block->pack_variant);
    }
    printf("\n");
}

static uint32_t fic_repo_valid_subchannel_count(const dab_fic_database_t *db) {
    uint32_t i;
    uint32_t count = 0u;

    if (db == 0) {
        return 0u;
    }
    for (i = 0u; i < DAB_FIC_MAX_SUBCHANNELS; ++i) {
        if (db->subchannels[i].valid != 0u) {
            count++;
        }
    }
    return count;
}

static const dab_fic_label_t *fic_repo_find_label(const dab_fic_database_t *db,
                                                  dab_fic_label_scope_t scope,
                                                  uint8_t is_programme,
                                                  uint32_t sid,
                                                  uint16_t eid,
                                                  uint8_t scids) {
    uint16_t i;

    if (db == 0) {
        return 0;
    }
    for (i = 0u; i < db->label_count; ++i) {
        const dab_fic_label_t *label = &db->labels[i];

        if (label->used == 0u || label->scope != scope) {
            continue;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_ENSEMBLE && label->eid == eid) {
            return label;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_SERVICE &&
            label->is_programme == is_programme &&
            label->sid == sid) {
            return label;
        }
        if (scope == DAB_FIC_LABEL_SCOPE_COMPONENT &&
            label->is_programme == is_programme &&
            label->sid == sid &&
            label->scids == scids) {
            return label;
        }
    }
    return 0;
}

static void print_fic_repo_summary(const dab_fic_repo_t *repo) {
    const dab_fic_database_t *db;
    const dab_fic_label_t *ensemble_label;
    uint32_t subchannel_count;
    uint32_t subchid;
    uint16_t service_index;

    if (repo == 0) {
        return;
    }

    db = &repo->current;
    subchannel_count = fic_repo_valid_subchannel_count(db);
    ensemble_label = fic_repo_find_label(db,
                                         DAB_FIC_LABEL_SCOPE_ENSEMBLE,
                                         0u,
                                         0u,
                                         db->eid,
                                         0u);

    printf("fic_repo eid=0x%04x subchannels=%u services=%u labels=%u",
           db->eid,
           subchannel_count,
           db->service_count,
           db->label_count);
    if (ensemble_label != 0 && ensemble_label->text[0] != '\0') {
        printf(" ensemble_label=\"%s\"", ensemble_label->text);
    }
    printf("\n");

    for (subchid = 0u; subchid < DAB_FIC_MAX_SUBCHANNELS; ++subchid) {
        const dab_fic_subchannel_t *subchannel = &db->subchannels[subchid];

        if (subchannel->valid == 0u) {
            continue;
        }
        if (subchannel->protection_kind == DAB_FIC_PROTECTION_UEP) {
            printf(" fic_subch id=%u start_cu=%u kind=UEP table=%u index=%u\n",
                   subchannel->subchid,
                   subchannel->start_cu,
                   subchannel->table_switch,
                   subchannel->protection_index);
        } else {
            printf(" fic_subch id=%u start_cu=%u kind=EEP option=%u level=%u size_cu=%u\n",
                   subchannel->subchid,
                   subchannel->start_cu,
                   subchannel->protection_option,
                   subchannel->protection_level,
                   subchannel->size_cu);
        }
    }

    for (service_index = 0u; service_index < db->service_count; ++service_index) {
        const dab_fic_service_t *service = &db->services[service_index];
        const dab_fic_label_t *service_label =
            fic_repo_find_label(db,
                                DAB_FIC_LABEL_SCOPE_SERVICE,
                                service->is_programme,
                                service->sid,
                                0u,
                                0u);

        if (service->component_count > 0u) {
            const dab_fic_service_component_t *component = &service->components[0];
            printf(" fic_service sid=0x%0*x programme=%u caid=%u comps=%u label=\"%s\" first_comp=(tmid=%u type=%u",
                   service->is_programme != 0u ? 4 : 8,
                   service->sid,
                   service->is_programme,
                   service->caid,
                   service->component_count,
                   service_label != 0 ? service_label->text : "",
                   component->tmid,
                   component->sc_type);
            if (component->has_subchid != 0u) {
                printf(" subch=%u", component->subchid);
            }
            printf(")\n");
        } else {
            printf(" fic_service sid=0x%0*x programme=%u caid=%u comps=0 label=\"%s\"\n",
                   service->is_programme != 0u ? 4 : 8,
                   service->sid,
                   service->is_programme,
                   service->caid,
                   service_label != 0 ? service_label->text : "");
        }
    }
}

static int process_variant(dab_fic_state_t *state,
                           uint64_t frame_start_sample,
                           const int8_t *src,
                           fic_layout_t layout,
                           int invert,
                           const dab_fic_debug_report_t *report) {
    int8_t work[DAB_TFRAME_FIC_SOFTBITS];
    uint32_t block_index;

    if (state == 0 || src == 0 || report == 0) {
        return -1;
    }

    make_layout_copy(src, layout, work);
    if (invert != 0) {
        invert_softbits(work);
    }

    for (block_index = 0u; block_index < DAB_FIC_BLOCKS_PER_FRAME; ++block_index) {
        const dab_fic_debug_block_t *block = &report->blocks[block_index];

        if (polarity0_pack0_valid_fibs(block) == 0u) {
            memset(work + (block_index * DAB_FIC_DEPARTITIONED_SOFTBITS),
                   0,
                   DAB_FIC_DEPARTITIONED_SOFTBITS);
        }
    }

    return dab_fic_process_frame(state, frame_start_sample, work, DAB_TFRAME_FIC_SOFTBITS);
}

int main(int argc, char **argv) {
    int8_t raw_fic[DAB_TFRAME_FIC_SOFTBITS];
    fic_layout_score_t scores[FIC_LAYOUT_COUNT];
    fic_layout_score_t *best = 0;
    dab_fic_state_t state;
    long file_size = 0;
    uint32_t layout;
    uint64_t frame_start_sample = 0u;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s /path/to/raw_softbits_i8.bin [frame_start_sample]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 3) {
        char *end = 0;
        frame_start_sample = strtoull(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            fprintf(stderr, "invalid frame_start_sample: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    if (read_fic_prefix(argv[1], raw_fic, &file_size) != 0) {
        fprintf(stderr,
                "failed to read at least %u FIC softbits from %s\n",
                (unsigned int)DAB_TFRAME_FIC_SOFTBITS,
                argv[1]);
        return EXIT_FAILURE;
    }

    for (layout = 0u; layout < FIC_LAYOUT_COUNT; ++layout) {
        if (score_layout(raw_fic, (fic_layout_t)layout, &scores[layout]) != 0) {
            fprintf(stderr, "failed to analyze layout %s\n", layout_name((fic_layout_t)layout));
            return EXIT_FAILURE;
        }
        if (best == 0 ||
            scores[layout].union_valid_fibs > best->union_valid_fibs ||
            (scores[layout].union_valid_fibs == best->union_valid_fibs &&
             scores[layout].normal.total_valid_fibs > best->normal.total_valid_fibs)) {
            best = &scores[layout];
        }
    }

    printf("input=%s bytes=%ld fic_softbits=%u\n",
           argv[1],
           file_size,
           (unsigned int)DAB_TFRAME_FIC_SOFTBITS);
    for (layout = 0u; layout < FIC_LAYOUT_COUNT; ++layout) {
        char name[64];

        printf("layout=%s union_parseable_fibs=%u\n",
               layout_name(scores[layout].layout),
               scores[layout].union_valid_fibs);
        snprintf(name, sizeof(name), "  %s", layout_name(scores[layout].layout));
        print_report_line(name, &scores[layout].normal);
        snprintf(name, sizeof(name), "  %s_inverted", layout_name(scores[layout].layout));
        print_report_line(name, &scores[layout].inverted);
    }

    if (best == 0 || best->union_valid_fibs == 0u) {
        fprintf(stderr, "no parseable FIC FIBs found\n");
        return EXIT_FAILURE;
    }

    if (dab_fic_init(&state) != 0) {
        fprintf(stderr, "fic init failed\n");
        return EXIT_FAILURE;
    }
    if (process_variant(&state, frame_start_sample, raw_fic, best->layout, 0, &best->normal) != 0 ||
        process_variant(&state, frame_start_sample, raw_fic, best->layout, 1, &best->inverted) != 0) {
        fprintf(stderr, "fic decode failed\n");
        return EXIT_FAILURE;
    }

    printf("selected_layout=%s union_parseable_fibs=%u total_valid_fibs_parsed=%u\n",
           layout_name(best->layout),
           best->union_valid_fibs,
           state.total_valid_fibs);
    print_fic_repo_summary(&state.repo);

    return EXIT_SUCCESS;
}
