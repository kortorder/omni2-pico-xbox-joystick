#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Narrow host substitute for the pinned BTstack TLV interface used by settings.
typedef struct {
    int (*get_tag)(void *, uint32_t, uint8_t *, uint32_t);
    int (*store_tag)(void *, uint32_t, const uint8_t *, uint32_t);
    void (*delete_tag)(void *, uint32_t);
} btstack_tlv_t;
void btstack_tlv_get_instance(const btstack_tlv_t **, void **);
#ifdef __cplusplus
}
#endif
