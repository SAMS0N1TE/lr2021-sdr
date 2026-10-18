#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_INVALID_VERSION 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_INVALID_ARG 4
int nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_u32(nvs_handle_t, const char *, uint32_t);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_commit(nvs_handle_t);
