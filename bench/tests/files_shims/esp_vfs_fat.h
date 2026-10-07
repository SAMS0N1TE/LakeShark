#ifndef FILES_TEST_FAT_H
#define FILES_TEST_FAT_H
#include "esp_err.h"
#include <stdint.h>
esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *free_b);
#endif
