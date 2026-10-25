#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { unsigned char *psegment;uint32_t svaddr;int (*entry)(int,char **); } esp_elf_t;
int esp_elf_init(esp_elf_t *elf);
int esp_elf_relocate(esp_elf_t *elf,const uint8_t *bytes);
void esp_elf_deinit(esp_elf_t *elf);
void elf_set_symbol_resolver(uintptr_t (*fn)(const char *));
