/* 资源包解析 —— 纯逻辑，不依赖硬件，可在主机上单独测试 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PET_PACK_MAGIC 0x50544550u /* "PETP" 小端 */
#define PET_PACK_VERSION 1

typedef struct {
    char name[12];
    uint8_t loop;
    uint16_t frame_count;
    uint32_t table_off; /* uint32 数组，每帧字节数 */
    uint32_t data_off;  /* 第一帧 JPEG 起始 */
} pet_pack_anim_t;

typedef struct {
    const uint8_t *base;
    size_t size;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t anim_count;
    pet_pack_anim_t anims[8];
} pet_pack_t;

esp_err_t pet_pack_parse(const uint8_t *data, size_t size, pet_pack_t *out);

/* 返回动画索引，找不到返回 -1 */
int pet_pack_find(const pet_pack_t *p, const char *name);

/* 取第 n 帧的 JPEG 指针与长度 */
esp_err_t pet_pack_frame(const pet_pack_t *p, int anim_idx, uint16_t n,
                         const uint8_t **jpeg, uint32_t *len);
