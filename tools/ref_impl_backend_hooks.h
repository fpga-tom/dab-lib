#ifndef DAB_TOOLS_REF_IMPL_BACKEND_HOOKS_H
#define DAB_TOOLS_REF_IMPL_BACKEND_HOOKS_H

#include <stdint.h>

#define DAB_REF_BACKEND_HOOKS_MAGIC 0x44524248u

typedef int (*dab_ref_dabplus_au_callback_t)(void *user,
                                             unsigned long superframe_index,
                                             unsigned int au_index,
                                             unsigned int au_count,
                                             const unsigned char *data,
                                             unsigned long len);

typedef struct {
    uint32_t magic;
    dab_ref_dabplus_au_callback_t on_dabplus_au;
    void *user;
} dab_ref_backend_hooks_t;

#endif
