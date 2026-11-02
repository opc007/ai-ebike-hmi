#include "pet_pack.h"

#include <string.h>

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

esp_err_t pet_pack_parse(const uint8_t *data, size_t size, pet_pack_t *out) {
    if (!data || !out) return ESP_ERR_INVALID_ARG;
    if (size < 20) return ESP_ERR_INVALID_SIZE;

    uint32_t magic = rd32(data);
    if (magic != PET_PACK_MAGIC) return ESP_ERR_INVALID_RESPONSE;

    uint8_t version = data[4];
    if (version != PET_PACK_VERSION) return ESP_ERR_NOT_SUPPORTED;

    uint8_t n = data[5];
    if (n == 0 || n > 8) return ESP_ERR_INVALID_SIZE;

    uint32_t total = rd32(data + 16);
    if (total > size) return ESP_ERR_INVALID_SIZE;

    memset(out, 0, sizeof(*out));
    out->base = data;
    out->size = total;
    out->width = rd16(data + 6);
    out->height = rd16(data + 8);
    out->fps = data[10];
    out->anim_count = n;

    if (out->fps == 0) out->fps = 20;
    if (out->width == 0 || out->height == 0) return ESP_ERR_INVALID_SIZE;

    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *e = data + 20 + (size_t)i * 26;
        if ((size_t)(e - data) + 26 > total) return ESP_ERR_INVALID_SIZE;

        pet_pack_anim_t *a = &out->anims[i];
        memcpy(a->name, e, 12);
        a->name[11] = '\0';
        a->loop = e[12];
        a->frame_count = rd16(e + 14);
        a->table_off = rd32(e + 18);
        a->data_off = rd32(e + 22);

        if (a->frame_count == 0) return ESP_ERR_INVALID_SIZE;
        if (a->table_off + 4u * a->frame_count > total) return ESP_ERR_INVALID_SIZE;
        if (a->data_off >= total) return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

int pet_pack_find(const pet_pack_t *p, const char *name) {
    if (!p || !name) return -1;
    for (int i = 0; i < p->anim_count; i++) {
        if (strncmp(p->anims[i].name, name, sizeof(p->anims[i].name)) == 0) return i;
    }
    return -1;
}

esp_err_t pet_pack_frame(const pet_pack_t *p, int anim_idx, uint16_t n,
                         const uint8_t **jpeg, uint32_t *len) {
    if (!p || anim_idx < 0 || anim_idx >= p->anim_count || !jpeg || !len)
        return ESP_ERR_INVALID_ARG;

    const pet_pack_anim_t *a = &p->anims[anim_idx];
    if (n >= a->frame_count) return ESP_ERR_INVALID_ARG;

    /* 帧长表：前 n 项累加即为该帧数据偏移 */
    uint32_t off = a->data_off;
    for (uint16_t i = 0; i < n; i++) {
        off += rd32(p->base + a->table_off + 4u * i);
        if (off >= p->size) return ESP_ERR_INVALID_SIZE;
    }
    uint32_t l = rd32(p->base + a->table_off + 4u * n);
    if (off + l > p->size) return ESP_ERR_INVALID_SIZE;

    *jpeg = p->base + off;
    *len = l;
    return ESP_OK;
}
