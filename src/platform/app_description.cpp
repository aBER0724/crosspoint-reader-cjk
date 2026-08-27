#include <esp_app_desc.h>

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

extern const __attribute__((section(".rodata_desc"))) esp_app_desc_t esp_app_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
    .secure_version = 0,
    .reserv1 = {0, 0},
    .version = CROSSPOINT_VERSION,
    .project_name = "crosspoint-reader-cjk",
    .time = __TIME__,
    .date = __DATE__,
    .idf_ver = IDF_VER,
    .app_elf_sha256 = {0},
    .min_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
    .max_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
    .mmu_page_size = 31 - __builtin_clz(CONFIG_MMU_PAGE_SIZE),
    .reserv3 = {0, 0, 0},
    .reserv2 = {0},
};
